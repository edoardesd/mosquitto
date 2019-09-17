/*
Copyright (c) 2009-2019 Roger Light <roger@atchoo.org>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.

The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.

Contributors:
   Roger Light - initial implementation and documentation.
*/

#include "config.h"

#include <assert.h>
#include <string.h>

#ifdef WIN32
#  include <winsock2.h>
#  include <aclapi.h>
#  include <io.h>
#  include <lmcons.h>
#else
#  include <sys/stat.h>
#endif

#if !defined(WITH_TLS) && defined(__linux__) && defined(__GLIBC__)
#  if __GLIBC_PREREQ(2, 25)
#    include <sys/random.h>
#    define HAVE_GETRANDOM 1
#  endif
#endif

#ifdef WITH_TLS
#  include <openssl/bn.h>
#  include <openssl/rand.h>
#endif

#ifdef WITH_BROKER
#include "mosquitto_broker_internal.h"
#endif

#include "mosquitto.h"
#include "memory_mosq.h"
#include "net_mosq.h"
#include "send_mosq.h"
#include "time_mosq.h"
#include "tls_mosq.h"
#include "util_mosq.h"

#ifdef WITH_WEBSOCKETS
#include <libwebsockets.h>
#endif



#ifdef WITH_BROKER
int init_list(PORT_LIST** head, char *type)
{
    *head = NULL;
    return MOSQ_ERR_SUCCESS;
}

void print_list(PORT_LIST* head, char *type)
{
    PORT_LIST * temp;
    fprintf(stdout, "List %s:", type);
    for (temp = head; temp; temp = temp->next){
        fprintf(stdout, " %d,", temp->broker.port);
    }
    fprintf(stdout, "\n");
}

PORT_LIST* add(PORT_LIST* node, BROKER broker)
{
    PORT_LIST* temp = (PORT_LIST*) malloc(sizeof (PORT_LIST));
    if (temp == NULL) {
        exit(EXIT_FAILURE);
        //return MOSQ_ERR_NOMEM; // no memory available
    }
    temp->broker = broker;
    temp->next = node;
    node = temp;
    log__printf(NULL, MOSQ_LOG_INFO, "Broker %d, added!", broker.port);
    return node;
}

bool in_list(PORT_LIST* head, char *address, int port)
{
    PORT_LIST* current = head;
    while(current!= NULL){
        if(current->broker.port == port) return true;
        current = current->next;
    }
    return false;
}

int remove_node(PORT_LIST* head)
{
    int temp_port = 0;
    
    PORT_LIST* temp = (PORT_LIST*) malloc(sizeof (PORT_LIST));
    if (temp == NULL) {
        return MOSQ_ERR_NOMEM;
        //exit(EXIT_FAILURE); // no memory available
    }
    temp = head;
    if(temp!=NULL){
        temp_port = head->broker.port;
        temp = head->next;
        head->next = head->next->next;
        free(temp);
    }
    return temp_port;
}

int delete_root(PORT_LIST **head){
    PORT_LIST *curr;
    
    curr = (*head)->next;
    free(*head);
    *head = curr;

    return 0;
}

int set__ports(struct mosquitto__stp *status, int msg_root_port, int msg_root_pid, int msg_distance, int msg_port, int msg_pid)
{
    int my_root_port, my_root_pid;
    int my_distance;
    int my_port, my_pid;
    
    my_root_port = status->my_root->port;
    my_root_pid = status->my_root->res->pid;
    my_distance = status->distance;
    my_port = status->my->port;
    my_pid = status->my->res->pid;
    
    if(my_root_pid < msg_root_pid){
        //check distance if less it's a DP else let's see
        log__printf(NULL, MOSQ_LOG_INFO, "SET designated 1");
        return DESIGNATED_PORT;
    }
    
    if(my_root_pid == msg_root_pid){
        if(my_distance < msg_distance){
            if(my_pid < msg_pid){
                log__printf(NULL, MOSQ_LOG_INFO, "SET designated 2");
                return DESIGNATED_PORT; //not always
            }else if(my_pid > msg_pid){
                log__printf(NULL, MOSQ_LOG_INFO, "SET BLOCK 1");
                return BLOCKED_PORT;
            }else{
                return NO_PORT;
            }
        } else if(my_distance > msg_distance){
            log__printf(NULL, MOSQ_LOG_INFO, "SET ROOT 1");
            return ROOT_PORT;
        } else if(my_distance == msg_distance){
            if(my_pid < msg_pid){
                log__printf(NULL, MOSQ_LOG_INFO, "SET designated 2");
                return DESIGNATED_PORT;
            }else if(my_pid > msg_pid){
                log__printf(NULL, MOSQ_LOG_INFO, "SET BLOCK 1");
                return BLOCKED_PORT;
            }else{
                return NO_PORT;
            }
        } else{
            log__printf(NULL, MOSQ_LOG_INFO, "SET NO 1");
            return NO_PORT;
        }
    }
    
    if(my_root_pid > msg_root_pid){
        log__printf(NULL, MOSQ_LOG_INFO, "SET root 2");
        return ROOT_PORT;
    }
    
    log__printf(NULL, MOSQ_LOG_INFO, "SET NO 2");
    return NO_PORT;
}


int update__stp_properties(struct mosquitto_db *db, struct mosquitto__bridge *bridge, struct mosquitto__bpdu__packet *packet)
{
    int origin_port, claimed_root_port;
    int origin_pid, root_distance;
    int claimed_root_pid;
    origin_port = atoi(packet->origin_port);
    claimed_root_port = atoi(packet->root_port);
    origin_pid = atoi(packet->origin_pid);
    root_distance = atoi(packet->distance);
    claimed_root_pid = atoi(packet->root_pid);
    
    int my_pid = db->stp->my->res->pid;
    //int my_root_pid = db->stp->my_root->res->pid;
    
    int port_next_status = NO_PORT;
    
    
    BROKER temp;
    
    //log__printf(NULL, MOSQ_LOG_DEBUG, "-> RECV [r(%d, %d), d(%d), o(%d, %d)]", claimed_root_port, claimed_root_pid, root_distance, origin_port, origin_pid);
    //log__printf(NULL, MOSQ_LOG_DEBUG, "-> OWN [r(%d, %d), d(%d), o(%d, %d)]", db->stp->my_root->port, db->stp->my_root->res->pid, db->stp->distance, db->stp->my->port,  db->stp->my->res->pid);
   
    /* ERROR PART */
    /* Origin and node = same address */
    if(db->stp->my->address == packet->origin_address && db->stp->my->port == origin_port){
        log__printf(NULL, MOSQ_LOG_WARNING, "Packet coming from the same address and port of the broker itself");
        if(db->stp->my->_id == packet->origin_id){
            log__printf(NULL, MOSQ_LOG_WARNING, "...and even the ID is the same");
        }
        return MOSQ_ERR_STP;
    }
    
    if(my_pid == origin_pid){
        log__printf(NULL, MOSQ_LOG_WARNING, "Same PID/ADDRESS");
        return MOSQ_ERR_STP;
    }
    
    port_next_status = set__ports(db->stp, claimed_root_port, claimed_root_pid, root_distance, origin_port, origin_pid);
   
//    if(bridge->port_status == port_next_status){
//        log__printf(NULL, MOSQ_LOG_INFO, "[PORTS] Nothing to update, go on.");
//        return MOSQ_ERR_SUCCESS;
//    }else{
//        log__printf(NULL, MOSQ_LOG_INFO, "[PORTS] Update ports.");
//        bridge->port_status = port_next_status;
//    }
    
    temp.address = "NULL";
    temp.port = origin_port;
    int old_root;
    switch (port_next_status) {
        case DESIGNATED_PORT:
            log__printf(NULL, MOSQ_LOG_INFO, "Port %d is DESIGNATED", temp.port);
            if(!in_list(bridge->designated_ports, NULL, temp.port)){
                bridge->designated_ports = add(bridge->designated_ports, temp);
            }
            break;
        case ROOT_PORT:
            db->stp->my_root->res->pid = claimed_root_pid;
            db->stp->my_root->port = claimed_root_port;
            db->stp->distance = root_distance + 1;
            if(packet->origin_id){
                db->stp->my_root->_id = packet->origin_id;
            }
            if(packet->origin_address){
                db->stp->my_root->address = packet->origin_address;
            }
            /* Add in root port list */
            log__printf(NULL, MOSQ_LOG_INFO, "Port %d is ROOT", temp.port);
            if(!in_list(bridge->root_ports, NULL, temp.port)){
                /* Empty_list + obtain old root */
                old_root = remove_node(bridge->root_ports);
                // TODO strange cases
                if(old_root){ //set old root as block
                    temp.port = old_root;
                    log__printf(NULL, MOSQ_LOG_INFO, "Port %d is BLOCK", temp.port);
                    if(!in_list(bridge->block_ports, NULL, temp.port)){
                        temp.address = NULL;
                        bridge->block_ports = add(bridge->block_ports, temp);
                    }
                }
                bridge->root_ports = add(bridge->root_ports, temp);
            }
            break;
        case BLOCKED_PORT:
            /* Add in block port list */
            log__printf(NULL, MOSQ_LOG_INFO, "Port %d is BLOCK", temp.port);
            if(!in_list(bridge->block_ports, NULL, temp.port)){
                bridge->block_ports = add(bridge->block_ports, temp);
            }
            break;
        case NO_CHANGE:
            break;
        case NO_PORT:
            log__printf(NULL, MOSQ_LOG_WARNING, "NO PORT error, impossible to have %d without port.", db->stp->my->port);
            return MOSQ_ERR_STP;
            break;
        default:
            log__printf(NULL, MOSQ_LOG_WARNING, "Wrong port status for %d.", db->stp->my->port);
            return MOSQ_ERR_STP;
            break;
    }
    
    print_list(bridge->designated_ports, "DESIGNATED");
    print_list(bridge->root_ports, "ROOT");
    print_list(bridge->block_ports, "BLOCK");

    return MOSQ_ERR_SUCCESS;
}
    
    
// /*
// *    UPDATE PART */
///* Root < RECV_node */
///*  if(my_root_pid < claimed_root_pid){
//        log__printf(NULL, MOSQ_LOG_DEBUG, "rootPID < RECEIVED pid");
//        //Current node is the root for RECV/SRC node
//        //set port as DESIGNATED PORT -> TODO
//        bridge->port_status = designated_port; //non always?
//        log__printf(NULL, MOSQ_LOG_DEBUG, "[PORT] set %d as DP for %s", bridge->addresses->port, bridge->local_clientid);
//        log__printf(NULL, MOSQ_LOG_DEBUG, "Message coming from a child???");
//        log__printf(NULL, MOSQ_LOG_DEBUG, "-------------------------------------------");
//        return MOSQ_ERR_SUCCESS;
//    }
//
//    /* Root == RECV node */
//    /* Impossible now, CHECK!! */
//    if(my_root_pid == claimed_root_pid){
//        //Current node is tie with the RECV node
//        /* The shorter DISTANCE won, the shorter distance do nothing */
//        if(db->stp->distance < root_distance){
//            bridge->port_status = designated_port; //non always
//            log__printf(NULL, MOSQ_LOG_DEBUG, "[PORT] set %d as DP for %s", bridge->addresses->port, bridge->local_clientid);
//        }
//        if(db->stp->distance == root_distance){
//            if(db->stp->my->port < origin_port){
//                 bridge->port_status = designated_port;
//                 log__printf(NULL, MOSQ_LOG_DEBUG, "[PORT] set %d as DP for %s", bridge->addresses->port, bridge->local_clientid);
//            }else{
//                /* TO FIX */
//              bridge->port_status = block_port;
//                BROKER to_block;
//                to_block.address = "NULL";
//                to_block.port = origin_port;
//                bridge->block_ports = add(bridge->block_ports, to_block);
//                    log__printf(NULL, MOSQ_LOG_DEBUG, "[PORT] set %d as BLOCK P for %s", bridge->addresses->port, bridge->local_clientid);
//
//                }
//                print_list(bridge->block_ports);
//            }
//        }
//
//
//        if(db->stp->distance > root_distance + 1){
//            log__printf(NULL, MOSQ_LOG_DEBUG, "NEW ROOT HAS BEEN FOUNDED");
//            //UPDATE and use the other node as best path
//            db->stp->my_root->res->pid = origin_pid;
//            if(origin_port){
//                db->stp->my_root->port = origin_port;
//            }
//            if(packet->origin_id){
//                db->stp->my_root->_id = packet->origin_id;
//            }
//            if(packet->origin_address){
//                db->stp->my_root->address = packet->origin_address;
//            }
//            /* Set port as ROOT port */ //TODO
//            bridge->port_status = king_port; // --> set old port as blocked port
//            log__printf(NULL, MOSQ_LOG_DEBUG, "[PORT] set %d as RP for %s", bridge->addresses->port, bridge->remote_clientid);
//        }
//        log__printf(NULL, MOSQ_LOG_DEBUG, "-------------------------------------------");
//        return MOSQ_ERR_SUCCESS;
//    }
//
//    if(my_root_pid > claimed_root_pid){
//        /* Check resources, update only if the resources are better than the current broker and the root current broker */
//        log__printf(NULL, MOSQ_LOG_DEBUG, "Message coming from the root, update values...");
//        if(packet->root_address){
//            db->stp->my_root->address = packet->origin_address;
//        }
//        if(packet->root_id){
//            db->stp->my_root->_id = packet->origin_id;
//        }
//        db->stp->my_root->port = origin_port;
//        db->stp->distance = root_distance+1;
//
//        /* Update resources */
//        db->stp->my_root->res->pid = origin_pid;
//
//        /* Set port as ROOT port */ //TODO
//        bridge->port_status = king_port; // --> set old port as blocked port
//        log__printf(NULL, MOSQ_LOG_DEBUG, "[PORT] set %d as RP for %s", bridge->addresses->port, db->config->bridges->remote_clientid);
//
//        log__printf(NULL, MOSQ_LOG_DEBUG, "-------------------------------------------");
//        return MOSQ_ERR_SUCCESS;
//    }
//    log__printf(NULL, MOSQ_LOG_DEBUG, "-------------------------------------------");
//    return MOSQ_ERR_STP;
//}
//*/
#endif

#ifdef WITH_BROKER
int mosquitto__check_keepalive(struct mosquitto_db *db, struct mosquitto *mosq)
#else
int mosquitto__check_keepalive(struct mosquitto *mosq)
#endif
{
	time_t next_msg_out;
	time_t last_msg_in;
	time_t now = mosquitto_time();
#ifndef WITH_BROKER
	int rc;
#endif

	assert(mosq);
#if defined(WITH_BROKER) && defined(WITH_BRIDGE)
	/* Check if a lazy bridge should be timed out due to idle. */
	if(mosq->bridge && mosq->bridge->start_type == bst_lazy
				&& mosq->sock != INVALID_SOCKET
				&& now - mosq->next_msg_out - mosq->keepalive >= mosq->bridge->idle_timeout){

		log__printf(NULL, MOSQ_LOG_NOTICE, "Bridge connection %s has exceeded idle timeout, disconnecting.", mosq->id);
		net__socket_close(db, mosq);
		return MOSQ_ERR_SUCCESS;
	}
#endif
	pthread_mutex_lock(&mosq->msgtime_mutex);
	next_msg_out = mosq->next_msg_out;
	last_msg_in = mosq->last_msg_in;
	pthread_mutex_unlock(&mosq->msgtime_mutex);
	if(mosq->keepalive && mosq->sock != INVALID_SOCKET &&
			(now >= next_msg_out || now - last_msg_in >= mosq->keepalive)){

		if(mosq->state == mosq_cs_connected && mosq->ping_t == 0){
#ifdef WITH_BROKER
			send__pingreq(db->stp, mosq);
#else
            send__pingreq(mosq);
#endif
			/* Reset last msg times to give the server time to send a pingresp */
			pthread_mutex_lock(&mosq->msgtime_mutex);
			mosq->last_msg_in = now;
			mosq->next_msg_out = now + mosq->keepalive;
			pthread_mutex_unlock(&mosq->msgtime_mutex);
		}else{
#ifdef WITH_BROKER
			net__socket_close(db, mosq);
#else
			net__socket_close(mosq);
			pthread_mutex_lock(&mosq->state_mutex);
			if(mosq->state == mosq_cs_disconnecting){
				rc = MOSQ_ERR_SUCCESS;
			}else{
				rc = MOSQ_ERR_KEEPALIVE;
			}
			pthread_mutex_unlock(&mosq->state_mutex);
			pthread_mutex_lock(&mosq->callback_mutex);
			if(mosq->on_disconnect){
				mosq->in_callback = true;
				mosq->on_disconnect(mosq, mosq->userdata, rc);
				mosq->in_callback = false;
			}
			if(mosq->on_disconnect_v5){
				mosq->in_callback = true;
				mosq->on_disconnect_v5(mosq, mosq->userdata, rc, NULL);
				mosq->in_callback = false;
			}
			pthread_mutex_unlock(&mosq->callback_mutex);

			return rc;
#endif
		}
	}
	return MOSQ_ERR_SUCCESS;
}

uint16_t mosquitto__mid_generate(struct mosquitto *mosq)
{
	/* FIXME - this would be better with atomic increment, but this is safer
	 * for now for a bug fix release.
	 *
	 * If this is changed to use atomic increment, callers of this function
	 * will have to be aware that they may receive a 0 result, which may not be
	 * used as a mid.
	 */
	uint16_t mid;
	assert(mosq);

	pthread_mutex_lock(&mosq->mid_mutex);
	mosq->last_mid++;
	if(mosq->last_mid == 0) mosq->last_mid++;
	mid = mosq->last_mid;
	pthread_mutex_unlock(&mosq->mid_mutex);

	return mid;
}


#ifdef WITH_TLS
int mosquitto__hex2bin_sha1(const char *hex, unsigned char **bin)
{
	unsigned char *sha, tmp[SHA_DIGEST_LENGTH];

	if(mosquitto__hex2bin(hex, tmp, SHA_DIGEST_LENGTH) != SHA_DIGEST_LENGTH){
		return MOSQ_ERR_INVAL;
	}

	sha = mosquitto__malloc(SHA_DIGEST_LENGTH);
	memcpy(sha, tmp, SHA_DIGEST_LENGTH);
	*bin = sha;
	return MOSQ_ERR_SUCCESS;
}

int mosquitto__hex2bin(const char *hex, unsigned char *bin, int bin_max_len)
{
	BIGNUM *bn = NULL;
	int len;
	int leading_zero = 0;
	int start = 0;
	size_t i = 0;

	/* Count the number of leading zero */
	for(i=0; i<strlen(hex); i=i+2) {
		if(strncmp(hex + i, "00", 2) == 0) {
			leading_zero++;
			/* output leading zero to bin */
			bin[start++] = 0;
		}else{
			break;
		}
	}

	if(BN_hex2bn(&bn, hex) == 0){
		if(bn) BN_free(bn);
		return 0;
	}
	if(BN_num_bytes(bn) + leading_zero > bin_max_len){
		BN_free(bn);
		return 0;
	}

	len = BN_bn2bin(bn, bin + leading_zero);
	BN_free(bn);
	return len + leading_zero;
}
#endif

FILE *mosquitto__fopen(const char *path, const char *mode, bool restrict_read)
{
#ifdef WIN32
	char buf[4096];
	int rc;
	rc = ExpandEnvironmentStrings(path, buf, 4096);
	if(rc == 0 || rc > 4096){
		return NULL;
	}else{
		if (restrict_read) {
			HANDLE hfile;
			SECURITY_ATTRIBUTES sec;
			EXPLICIT_ACCESS ea;
			PACL pacl = NULL;
			char username[UNLEN + 1];
			int ulen = UNLEN;
			SECURITY_DESCRIPTOR sd;
			DWORD dwCreationDisposition;

			switch(mode[0]){
				case 'a':
					dwCreationDisposition = OPEN_ALWAYS;
					break;
				case 'r':
					dwCreationDisposition = OPEN_EXISTING;
					break;
				case 'w':
					dwCreationDisposition = CREATE_ALWAYS;
					break;
				default:
					return NULL;
			}

			GetUserName(username, &ulen);
			if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
				return NULL;
			}
			BuildExplicitAccessWithName(&ea, username, GENERIC_ALL, SET_ACCESS, NO_INHERITANCE);
			if (SetEntriesInAcl(1, &ea, NULL, &pacl) != ERROR_SUCCESS) {
				return NULL;
			}
			if (!SetSecurityDescriptorDacl(&sd, TRUE, pacl, FALSE)) {
				LocalFree(pacl);
				return NULL;
			}

			sec.nLength = sizeof(SECURITY_ATTRIBUTES);
			sec.bInheritHandle = FALSE;
			sec.lpSecurityDescriptor = &sd;

			hfile = CreateFile(buf, GENERIC_READ | GENERIC_WRITE, 0,
				&sec,
				dwCreationDisposition,
				FILE_ATTRIBUTE_NORMAL,
				NULL);

			LocalFree(pacl);

			int fd = _open_osfhandle((intptr_t)hfile, 0);
			if (fd < 0) {
				return NULL;
			}

			FILE *fptr = _fdopen(fd, mode);
			if (!fptr) {
				_close(fd);
				return NULL;
			}
			return fptr;

		}else {
			return fopen(buf, mode);
		}
	}
#else
	if (restrict_read) {
		FILE *fptr;
		mode_t old_mask;

		old_mask = umask(0077);
		fptr = fopen(path, mode);
		umask(old_mask);

		return fptr;
	}else{
		return fopen(path, mode);
	}
#endif
}

void util__increment_receive_quota(struct mosquitto *mosq)
{
	if(mosq->msgs_in.inflight_quota < mosq->msgs_in.inflight_maximum){
		mosq->msgs_in.inflight_quota++;
	}
}

void util__increment_send_quota(struct mosquitto *mosq)
{
	if(mosq->msgs_out.inflight_quota < mosq->msgs_out.inflight_maximum){
		mosq->msgs_out.inflight_quota++;
	}
}


void util__decrement_receive_quota(struct mosquitto *mosq)
{
	if(mosq->msgs_in.inflight_quota > 0){
		mosq->msgs_in.inflight_quota--;
	}
}

void util__decrement_send_quota(struct mosquitto *mosq)
{
	if(mosq->msgs_out.inflight_quota > 0){
		mosq->msgs_out.inflight_quota--;
	}
}


int util__random_bytes(void *bytes, int count)
{
	int rc = MOSQ_ERR_UNKNOWN;

#ifdef WITH_TLS
	if(RAND_bytes(bytes, count) == 1){
		rc = MOSQ_ERR_SUCCESS;
	}
#elif defined(HAVE_GETRANDOM)
	if(getrandom(bytes, count, 0) == count){
		rc = MOSQ_ERR_SUCCESS;
	}
#elif defined(WIN32)
	HCRYPTPROV provider;

	if(!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)){
		return MOSQ_ERR_UNKNOWN;
	}

	if(CryptGenRandom(provider, count, bytes)){
		rc = MOSQ_ERR_SUCCESS;
	}

	CryptReleaseContext(provider, 0);
#else
	int i;

	for(i=0; i<count; i++){
		((uint8_t *)bytes)[i] = (uint8_t )(random()&0xFF);
	}
	rc = MOSQ_ERR_SUCCESS;
#endif
	return rc;
}
