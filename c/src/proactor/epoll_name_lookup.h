/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#ifndef PROACTOR_NAME_LOOKUP_H
#define PROACTOR_NAME_LOOKUP_H

#include "epoll-internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Blocking name lookup - used by listeners and as fallback for connections.
 * Same signature as getaddrinfo.
 */
int pni_name_lookup_blocking(const char *host, const char *port, int flags, struct addrinfo **res);

/* Initialize name lookup subsystem. Returns false if error. */
bool pni_name_lookup_init(pname_lookup_t *nl, pn_proactor_t *p);

/* Cleanup name lookup subsystem. */
void pni_name_lookup_cleanup(pname_lookup_t *nl, pn_proactor_t *p);

/* Callback when lookup completes. Called with (user_data, addrinfo or NULL, gai_error). */
typedef void (*pni_nl_done_cb)(void *user_data, struct addrinfo *ai, int gai_error);

/* Start a name lookup. Returns true if started successfully. */
bool pni_name_lookup_start(pname_lookup_t *nl, const char *host, const char *port, void *user_data, pni_nl_done_cb done_cb);

/* Clear lookup state when forcing shutdown. */
void pni_name_lookup_forced_shutdown(pname_lookup_t *nl);

/* Process events. */
void pni_name_lookup_process_events(pname_lookup_t *nl);

#ifdef __cplusplus
}
#endif

#endif /* PROACTOR_NAME_LOOKUP_H */
