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

/* Asynchronous name lookup using c-ares. */

#include "epoll_name_lookup.h"

#include <limits.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
// Ares 1.27 doesn't include sys/select.h in ares.h but uses fd_set
// So even though we don't use that we still need to include it here
// before we include ares.h
#include <sys/select.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <ares.h>

// 0x012200 corresponds to version 1.34 of c-ares which added ares_process_fds
#if ARES_VERSION >= 0x012200
#  define CARES_HAVE_PROCESS_FDS 1
#else
#  define CARES_HAVE_PROCESS_FDS 0
#endif


/* Opaque context for async name lookup. */
typedef struct pni_nl_async_ctx {
  pmutex pending_mutex;
  struct ares_channeldata *channel;
  struct pni_nl_request *pending_head;
  int timerfd;  /* In inner epoll; fires for ares timeout, -1 when not in use */
} pni_nl_async_ctx_t;

static void nl_timerfd_set(int fd, uint64_t t_millis)
{
  struct itimerspec newt;
  memset(&newt, 0, sizeof(newt));
  if (t_millis > 0) {
    newt.it_value.tv_sec = t_millis / 1000;
    newt.it_value.tv_nsec = (t_millis % 1000) * 1000000;
  }
  timerfd_settime(fd, 0, &newt, NULL);
}

static void nl_timerfd_drain(int fd)
{
  uint64_t result = 0;
  __attribute__((unused)) ssize_t r = read(fd, &result, sizeof(result));
}

/* Set timerfd from ares next deadline; call after start or process_events. */
static void nl_update_ares_timer(pname_lookup_t *nl)
{
  pni_nl_async_ctx_t *ctx = (pni_nl_async_ctx_t *)nl->impl;
  if (!ctx || ctx->timerfd < 0) return;
  struct timeval tv;
  if (ares_timeout(ctx->channel, NULL, &tv) != NULL) {
    long ares_ms = (long)tv.tv_sec * 1000 + (int)(tv.tv_usec / 1000);
    if (ares_ms > 0 && ares_ms <= INT_MAX)
      nl_timerfd_set(ctx->timerfd, (uint64_t)ares_ms);
    else
      nl_timerfd_set(ctx->timerfd, 0);
  } else {
    nl_timerfd_set(ctx->timerfd, 0);
  }
}

typedef struct pni_nl_request {
  pni_nl_async_ctx_t *ctx;
  void *user_data;
  pni_nl_done_cb done_cb;
  struct pni_nl_request *next;
} pni_nl_request_t;

static void nl_sock_state_cb(void *data, ares_socket_t socket_fd, int readable, int writable)
{
  pname_lookup_t *nl = (pname_lookup_t *)data;
  int epfd = nl->epoll_name_lookup.fd;
  if (epfd < 0) return;
  struct epoll_event ev;
  ev.data.fd = socket_fd;
  int events = 0;
  if (readable) events |= EPOLLIN;
  if (writable) events |= EPOLLOUT;

  if (readable || writable) {
    ev.events = events;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, socket_fd, &ev) < 0) {
      epoll_ctl(epfd, EPOLL_CTL_ADD, socket_fd, &ev);
    }
  } else {
    epoll_ctl(epfd, EPOLL_CTL_DEL, socket_fd, NULL);
  }
}

static struct addrinfo *nl_ares_addrinfo_to_addrinfo(struct ares_addrinfo *result)
{
  struct addrinfo *ai_head = NULL;
  struct addrinfo *ai_prev = NULL;
  for (struct ares_addrinfo_node *node = result->nodes; node != NULL; node = node->ai_next) {
    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo)+node->ai_addrlen);
    if (!ai) break;
    ai->ai_family = node->ai_family;
    ai->ai_socktype = node->ai_socktype;
    ai->ai_protocol = node->ai_protocol;
    ai->ai_flags = node->ai_flags;
    ai->ai_addrlen = node->ai_addrlen;
    ai->ai_addr = (struct sockaddr *)(ai + 1);
    memcpy(ai->ai_addr, node->ai_addr, node->ai_addrlen);
    ai->ai_next = NULL;
    if (ai_prev) {
      ai_prev->ai_next = ai;
    } else {
      ai_head = ai;
    }
    ai_prev = ai;
  }
  return ai_head;
}

static int nl_ares_status_to_gai(int status)
{
  switch (status) {
  case ARES_ENOTFOUND:
  case ARES_ESERVICE:
    return EAI_NONAME;
  case ARES_ETIMEOUT:
    return EAI_AGAIN;
  case ARES_ESERVFAIL:
  case ARES_ECANCELLED:
  case ARES_EDESTRUCTION:
    return EAI_FAIL;
  case ARES_ENOMEM:
    return EAI_MEMORY;
  default:
    return EAI_NONAME;
  }
}

static void nl_ares_lookup_cb(void *data, int status, int timeouts, struct ares_addrinfo *result)
{
  pni_nl_request_t *req = (pni_nl_request_t *)data;
  pni_nl_async_ctx_t *ctx = req->ctx;

  lock(&ctx->pending_mutex);
  /* Unlink from pending list */
  {
    pni_nl_request_t **pp = &ctx->pending_head;
    while (*pp && *pp != req) pp = &(*pp)->next;
    if (*pp) *pp = req->next;
  }
  unlock(&ctx->pending_mutex);

  struct addrinfo *ai = NULL;
  int gai_error = EAI_NONAME;
  if (status == ARES_SUCCESS && result) {
    ai = nl_ares_addrinfo_to_addrinfo(result);
    gai_error = ai ? 0 : EAI_MEMORY;
  } else {
    gai_error = nl_ares_status_to_gai(status);
  }
  if (result) {
    ares_freeaddrinfo(result);
  }
  if (status != ARES_ECANCELLED && status != ARES_EDESTRUCTION) {
    req->done_cb(req->user_data, ai, gai_error);
  }
  free(req);
}

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
  task_init(&nl->task, NAME_LOOKUP, p);
  pni_nl_async_ctx_t *ctx = (pni_nl_async_ctx_t *)calloc(1, sizeof(*ctx));
  if (!ctx) return false;
  nl->impl = ctx;
  pmutex_init(&ctx->pending_mutex);
  ctx->pending_head = NULL;

  if (ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS) {
    free(ctx);
    nl->impl = NULL;
    return false;
  }
  if ((nl->epoll_name_lookup.fd = epoll_create1(0)) < 0) {
    ares_library_cleanup();
    return false;
  }
  struct ares_options options = {
    .sock_state_cb = nl_sock_state_cb,
    .sock_state_cb_data = nl
  };
  if (ares_init_options(&ctx->channel, &options, ARES_OPT_SOCK_STATE_CB) != ARES_SUCCESS) {
    close(nl->epoll_name_lookup.fd);
    nl->epoll_name_lookup.fd = -1;
    ares_library_cleanup();
    return false;
  }
  ctx->timerfd = -1;
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (tfd >= 0) {
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = tfd };
    if (epoll_ctl(nl->epoll_name_lookup.fd, EPOLL_CTL_ADD, tfd, &ev) == 0) {
      ctx->timerfd = tfd;
    } else {
      close(tfd);
      return false;
    }
  } else {
    return false;
  }
  /* Register inner epoll with main proactor epoll (fd already in nl->epoll_name_lookup) */
  nl->epoll_name_lookup.type = NAME_LOOKUP_EPOLL;
  nl->epoll_name_lookup.wanted = EPOLLIN;
  nl->epoll_name_lookup.polling = false;
  pmutex_init(&nl->epoll_name_lookup.barrier_mutex);
  if (!start_polling(&nl->epoll_name_lookup, p->epollfd)) {
    if (ctx->timerfd >= 0) {
      close(ctx->timerfd);
      ctx->timerfd = -1;
    }
    ares_destroy(ctx->channel);
    pmutex_finalize(&nl->epoll_name_lookup.barrier_mutex);
    close(nl->epoll_name_lookup.fd);
    nl->epoll_name_lookup.fd = -1;
    ares_library_cleanup();
    free(ctx);
    nl->impl = NULL;
    return false;
  }
  nl_update_ares_timer(nl);
  return true;
}

void pni_name_lookup_cleanup(pname_lookup_t *nl, pn_proactor_t *p)
{
  pni_nl_async_ctx_t *ctx = (pni_nl_async_ctx_t *)nl->impl;
  if (!ctx) return;
  if (nl->epoll_name_lookup.fd >= 0) {
    stop_polling(&nl->epoll_name_lookup, p->epollfd);
    if (ctx->timerfd >= 0) {
      close(ctx->timerfd);
      ctx->timerfd = -1;
    }
    ares_cancel(ctx->channel);
    ares_destroy(ctx->channel);
    pmutex_finalize(&nl->epoll_name_lookup.barrier_mutex);
    close(nl->epoll_name_lookup.fd);
    nl->epoll_name_lookup.fd = -1;
    ares_library_cleanup();
  }
  /* Free requests still on pending list */
  lock(&ctx->pending_mutex);
  while (ctx->pending_head) {
    pni_nl_request_t *req = ctx->pending_head;
    ctx->pending_head = req->next;
    free(req);
  }
  unlock(&ctx->pending_mutex);
  free(ctx);
  nl->impl = NULL;
}

bool pni_name_lookup_start(pname_lookup_t *nl, const char *host, const char *port, void *user_data, pni_nl_done_cb done_cb)
{
  pni_nl_async_ctx_t *ctx = (pni_nl_async_ctx_t *)nl->impl;

  if (!ctx || nl->epoll_name_lookup.fd < 0 || !done_cb)
    return false;
  /* ares can't cope with a NULL host; do a blocking lookup for that case. */
  if (host == NULL) {
    struct addrinfo *res = NULL;
    int gai_error = pni_name_lookup_blocking(host, port, 0, &res);
    done_cb(user_data, res, gai_error);
    return gai_error == 0;
  }
  pni_nl_request_t *req = (pni_nl_request_t *)malloc(sizeof(*req));
  if (!req) {
    done_cb(user_data, NULL, EAI_MEMORY);
    return false;
  }

  lock(&ctx->pending_mutex);
  *req = (pni_nl_request_t){
    .ctx = ctx,
    .user_data = user_data,
    .done_cb = done_cb,
    .next = ctx->pending_head,
  };
  ctx->pending_head = req;
  unlock(&ctx->pending_mutex);

  struct ares_addrinfo_hints hints = {
    .ai_flags = ARES_AI_V4MAPPED | ARES_AI_ADDRCONFIG,
    .ai_family = AF_UNSPEC,
    .ai_socktype = SOCK_STREAM,
  };
  ares_getaddrinfo(ctx->channel, host, port, &hints, nl_ares_lookup_cb, req);
  nl_update_ares_timer(nl);
  return true;
}

void pni_name_lookup_forced_shutdown(pname_lookup_t *nl)
{
  pni_nl_async_ctx_t *ctx = (pni_nl_async_ctx_t *)nl->impl;
  if (!ctx) return;
  lock(&ctx->pending_mutex);
  pni_nl_request_t **pp = &ctx->pending_head;
  while (*pp) {
    pni_nl_request_t *req = *pp;
    *pp = req->next;
    free(req);
    pp = &(*pp)->next;
  }
  unlock(&ctx->pending_mutex);
}

void pni_name_lookup_process_events(pname_lookup_t *nl)
{
  int epfd = nl->epoll_name_lookup.fd;
  if (epfd < 0) return;
  pni_nl_async_ctx_t *ctx = (pni_nl_async_ctx_t *)nl->impl;
  if (!ctx) return;
  struct epoll_event events[10];
  int n = epoll_wait(epfd, events, 10, 0);
  if (n < 0) return;
  int n_fds = 0;
  bool timer_fired = false;
#if CARES_HAVE_PROCESS_FDS
  ares_fd_events_t fd_events[10];
  for (int j = 0; j < n && n_fds < 10; j++) {
    int fd = events[j].data.fd;
    if (fd == ctx->timerfd) {
      nl_timerfd_drain(ctx->timerfd);
      timer_fired = true;
    } else {
      fd_events[n_fds].fd = fd;
      fd_events[n_fds].events = 0;
      if (events[j].events & EPOLLIN) fd_events[n_fds].events |= ARES_FD_EVENT_READ;
      if (events[j].events & EPOLLOUT) fd_events[n_fds].events |= ARES_FD_EVENT_WRITE;
      n_fds++;
    }
  }
  if (n_fds > 0)
    ares_process_fds(ctx->channel, fd_events, n_fds, 0);
  else if (timer_fired)
    ares_process_fds(ctx->channel, NULL, 0, 0);
#else
  for (int j = 0; j < n; j++) {
    int fd = events[j].data.fd;
    if (fd == ctx->timerfd) {
      nl_timerfd_drain(ctx->timerfd);
      timer_fired = true;
    } else {
      ares_socket_t r = (events[j].events & EPOLLIN) ? fd : ARES_SOCKET_BAD;
      ares_socket_t w = (events[j].events & EPOLLOUT) ? fd : ARES_SOCKET_BAD;
      ares_process_fd(ctx->channel, r, w);
      n_fds++;
    }
  }
  if (timer_fired && n_fds == 0)
    ares_process_fd(ctx->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
#endif
  nl_update_ares_timer(nl);
  if (nl->epoll_name_lookup.polling)
    rearm_polling(&nl->epoll_name_lookup, nl->task.proactor->epollfd);
}
