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
#ifndef UTIL_MOSQ_H
#define UTIL_MOSQ_H

#include <stdio.h>

#include "tls_mosq.h"
#include "mosquitto.h"
#include "mosquitto_internal.h"
#ifdef WITH_BROKER
#  include "mosquitto_broker_internal.h"
#endif

#ifdef WITH_BROKER
int mosquitto__check_keepalive(struct mosquitto_db *db, struct mosquitto *mosq);
#else
int mosquitto__check_keepalive(struct mosquitto *mosq);
#endif
uint16_t mosquitto__mid_generate(struct mosquitto *mosq);
FILE *mosquitto__fopen(const char *path, const char *mode, bool restrict_read);

#ifdef WITH_BROKER
int update__stp_properties(struct mosquitto_db *db, struct mosquitto__stp *stp, struct mosquitto__bridge *bridge, struct mosquitto__bpdu__packet *packet);
int stp__algorithm(struct mosquitto_db *db, struct mosquitto__stp *stp, struct mosquitto__bridge *context, struct mosquitto__bpdu__packet *packet);
int set__ports(struct mosquitto__stp *status, int msg_root_port, int msg_root_pid, int msg_distance, int msg_port, int msg_pid);
struct mosquitto__bpdu__packet *find_bridge(struct mosquitto_db *db, struct mosquitto__bpdu__packet *packet, int origin_port, int i);
bool check_repeated(struct mosquitto__bpdu__packet *stored_bpdu, struct mosquitto__bpdu__packet *packet);
struct mosquitto__bpdu__packet* init__bpdu(struct mosquitto_db *db, struct mosquitto__bpdu__packet *bpdu);
bool check_convergence(struct mosquitto_db *db, PORT_LIST *old_des, PORT_LIST *old_block, BROKER old_k);

#endif

#ifdef WITH_TLS
int mosquitto__hex2bin_sha1(const char *hex, unsigned char **bin);
int mosquitto__hex2bin(const char *hex, unsigned char *bin, int bin_max_len);
#endif

int util__random_bytes(void *bytes, int count);

void util__increment_receive_quota(struct mosquitto *mosq);
void util__increment_send_quota(struct mosquitto *mosq);
void util__decrement_receive_quota(struct mosquitto *mosq);
void util__decrement_send_quota(struct mosquitto *mosq);
#endif
