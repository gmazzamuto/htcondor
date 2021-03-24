/***************************************************************
 *
 * Copyright (C) 1990-2021, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/



#include "condor_common.h"
#include "condor_config.h"
#include "string_list.h"

#include "arcresource.h"
#include "arcjob.h"
#include "gridmanager.h"

std::map <std::string, ArcResource *>
    ArcResource::ResourcesByName;

std::string& ArcResource::HashName( const char *resource_name,
                                    const char *proxy_subject )
{
	static std::string hash_name;

	formatstr( hash_name, "arc %s#%s", resource_name,
			proxy_subject ? proxy_subject : "NULL" );

	return hash_name;
}

ArcResource *ArcResource::FindOrCreateResource( const char * resource_name,
												const Proxy *proxy )
{
	ArcResource *resource = NULL;
	const std::string &key = HashName( resource_name, proxy->subject->fqan );
	auto itr = ResourcesByName.find( key );
	if ( itr == ResourcesByName.end() ) {
		resource = new ArcResource( resource_name, proxy );
		ASSERT(resource);
		resource->Reconfig();
		ResourcesByName[key] = resource;
	} else {
		resource = itr->second;
		ASSERT(resource);
	}

	return resource;
}

ArcResource::ArcResource( const char *resource_name,
								  const Proxy *proxy )
	: BaseResource( resource_name )
{
	proxySubject = strdup( proxy->subject->subject_name );
	proxyFQAN = strdup( proxy->subject->fqan );

	gahp = NULL;

	std::string buff;
	formatstr( buff, "ARC/%s", proxyFQAN );

	gahp = new GahpClient( buff.c_str() );
	gahp->setNotificationTimerId( pingTimerId );
	gahp->setMode( GahpClient::normal );
	gahp->setTimeout( ArcJob::gahpCallTimeout );

	m_jobStatusTid = TIMER_UNSET;
	/*
	m_jobStatusTid = daemonCore->Register_Timer( 0,
							(TimerHandlercpp)&ArcResource::DoJobStatus,
							"ArcResource::DoJobStatus", (Service*)this );
	*/

	m_statusGahp = new GahpClient( buff.c_str() );
	m_statusGahp->setNotificationTimerId( m_jobStatusTid );
	m_statusGahp->setMode( GahpClient::normal );
	m_statusGahp->setTimeout( ArcJob::gahpCallTimeout );

	m_jobStatusActive = false;
}

ArcResource::~ArcResource()
{
	ResourcesByName.erase( HashName( resourceName, proxyFQAN ) );
	free( proxyFQAN );
	if ( proxySubject ) {
		free( proxySubject );
	}
	if ( gahp ) {
		delete gahp;
	}
	delete m_statusGahp;
	if ( m_jobStatusTid != TIMER_UNSET ) {
		daemonCore->Cancel_Timer( m_jobStatusTid );
	}
}

void ArcResource::Reconfig()
{
	BaseResource::Reconfig();

	gahp->setTimeout( ArcJob::gahpCallTimeout );
}

const char *ArcResource::ResourceType()
{
	return "arc";
}

const char *ArcResource::GetHashName()
{
	return HashName( resourceName, proxyFQAN ).c_str();
}

void ArcResource::PublishResourceAd( ClassAd *resource_ad )
{
	BaseResource::PublishResourceAd( resource_ad );

	resource_ad->Assign( ATTR_X509_USER_PROXY_SUBJECT, proxySubject );
	resource_ad->Assign( ATTR_X509_USER_PROXY_FQAN, proxyFQAN );

	gahp->PublishStats( resource_ad );
}

void ArcResource::DoPing( unsigned& ping_delay, bool& ping_complete,
							bool& ping_succeeded )
{
	int rc;

	if ( gahp->isInitialized() == false ) {
		dprintf( D_ALWAYS,"gahp server not up yet, delaying ping\n" );
		ping_delay = 5;
		return;
	}
	gahp->setNormalProxy( gahp->getMasterProxy() );
	if ( PROXY_IS_EXPIRED( gahp->getMasterProxy() ) ) {
		dprintf( D_ALWAYS,"proxy near expiration or invalid, delaying ping\n" );
		ping_delay = TIMER_NEVER;
		return;
	}

	ping_delay = 0;

	rc = gahp->arc_ping( resourceName );

	if ( rc == GAHPCLIENT_COMMAND_PENDING ) {
		ping_complete = false;
	} else if ( rc != 0 ) {
		ping_complete = true;
		ping_succeeded = false;
	} else {
		ping_complete = true;
		ping_succeeded = true;
	}
}

void ArcResource::DoJobStatus()
{
	if ( ( registeredJobs.IsEmpty() || resourceDown ) &&
		 m_jobStatusActive == false ) {
			// No jobs or we can't talk to the resource, so no point
			// in polling
		daemonCore->Reset_Timer( m_jobStatusTid, m_paramJobPollInterval );
		return;
	}

	if ( m_statusGahp->isStarted() == false ) {
		// The gahp isn't started yet. Wait a few seconds for a Job
		// object to start it and initialize x509 credentials.
		daemonCore->Reset_Timer( m_jobStatusTid, 5 );
		return;
	}

	daemonCore->Reset_Timer( m_jobStatusTid, TIMER_NEVER );

	StringList results;

	if ( m_jobStatusActive == false ) {

			// start ldap status command
		dprintf( D_FULLDEBUG, "Starting ldap poll: %s\n", resourceName );

		std::string ldap_server = resourceName;
		size_t pos = ldap_server.find_first_of( ':' );
		if ( pos != std::string::npos ) {
			ldap_server.erase( pos );
		}

		std::string filter;
		formatstr( filter, "(&(objectclass=nordugrid-job)(nordugrid-job-globalowner=%s))", proxySubject );
		int rc = m_statusGahp->nordugrid_ldap_query( ldap_server.c_str(), "mds-vo-name=local,o=grid", filter.c_str(), "nordugrid-job-globalid,nordugrid-job-status",
													 results );
		if ( rc != GAHPCLIENT_COMMAND_PENDING ) {
			dprintf( D_ALWAYS,
					 "gahp->nordugrid_ldap_query returned %d for resource %s\n",
					 rc, resourceName );
			EXCEPT( "nordugrid_ldap_query failed!" );
		}
		m_jobStatusActive = true;

	} else {

			// finish ldap status command
		int rc = m_statusGahp->nordugrid_ldap_query( NULL, NULL, NULL, NULL, results );

		if ( rc == GAHPCLIENT_COMMAND_PENDING ) {
			return;
		} else if ( rc != 0 ) {
			dprintf( D_ALWAYS,
					 "gahp->nordugrid_ldap_query returned %d for resource %s: %s\n",
					 rc, resourceName, m_statusGahp->getErrorString() );
			dprintf( D_ALWAYS, "Requesting ping of resource\n" );
			RequestPing( NULL );
		}

		if ( rc == 0 ) {
			const char *next_job_id = NULL;
			const char *next_status = NULL;
			const char *next_attr;
			std::string key;

			results.rewind();
			do {
				next_attr = results.next();

				if ( next_attr != NULL && *next_attr != '\0' ) {
						// Save the attributes we're interested in
					if ( !strncmp( next_attr, "nordugrid-job-globalid: ", 24 ) ) {
						next_job_id = next_attr;
					} else if ( !strncmp( next_attr, "nordugrid-job-status: ", 22 ) ) {
						next_status = next_attr;
					}
					continue;
				}
					// We just reached the end of a record. Process it.
					// If we don't have the attributes we expect, skip it.
				if ( next_job_id && next_status ) {
					int rc2;
					const char *id;
					BaseJob *base_job = NULL;
					ArcJob *job = NULL;
					id = strrchr( next_job_id, '/' );
					id = (id != NULL) ? (id + 1) : "";
					formatstr( key, "nordugrid %s %s", resourceName, id );
					rc2 = BaseJob::JobsByRemoteId.lookup( key, base_job );
					if ( rc2 == 0 && (job = dynamic_cast<ArcJob*>(base_job)) ) {
						job->NotifyNewRemoteStatus( strchr( next_status, ' ' ) + 1 );
					}
				}
				next_job_id = NULL;
				next_status = NULL;
			} while ( next_attr );

		}

		m_jobStatusActive = false;

		dprintf( D_FULLDEBUG, "ldap poll complete: %s\n", resourceName );

		daemonCore->Reset_Timer( m_jobStatusTid, m_paramJobPollInterval );
	}
}