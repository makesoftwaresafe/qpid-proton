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

/* Synchronous (blocking) name lookup using getaddrinfo. */

#include "epoll_name_lookup.h"

#include <netdb.h>
#include <string.h>

int pni_name_lookup_blocking(const char *host, const char *port, int flags, struct addrinfo **res)
{
  struct addrinfo hints = { 0 };
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_V4MAPPED | AI_ADDRCONFIG | flags;
  return getaddrinfo(host, port, &hints, res);
}

bool pni_name_lookup_init(pname_lookup_t *nl, pn_proactor_t *p)
{
  return true;
}

void pni_name_lookup_cleanup(pname_lookup_t *nl, pn_proactor_t *p)
{
}

bool pni_name_lookup_start(pname_lookup_t *nl, const char *host, const char *port, void *user_data, pni_nl_done_cb done_cb)
{
  struct addrinfo *res = NULL;
  int gai_error = pni_name_lookup_blocking(host, port, 0, &res);
  done_cb(user_data, res, gai_error);
  return gai_error == 0;
}

void pni_name_lookup_forced_shutdown(pname_lookup_t *nl)
{
}

void pni_name_lookup_process_events(pname_lookup_t *nl)
{
}
