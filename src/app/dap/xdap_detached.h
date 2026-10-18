/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_detached.h - Independent ownership and driving after DAP detach
 */
#ifndef XDAP_DETACHED_H
#define XDAP_DETACHED_H
#include "xdap_launch.h"

typedef struct XdapDetached XdapDetached;
/* The host owns a detached target independently of every DAP object. Poll on
 * one drive owner; original work/poll limits and authentic waits remain active.
 * A pending timer consumes no VM poll. Failure preserves ended and the owner. */
XR_FUNC XrXirStatus xdap_detached_poll(XdapDetached *,uint64_t quantum,bool *ended);
XR_FUNC bool xdap_detached_waiting(const XdapDetached *);
/* Copies only a genuine terminal result. The caller provides an empty owner. */
XR_FUNC XrXirValueStatus xdap_detached_result(const XdapDetached *,XrXirCallResult *);
/* Explicitly releases the target; active execution is stopped by its original
 * Instance owner. Consumes except on BUSY and returns actual cleanup status. */
XR_FUNC XrXirCallStatus xdap_detached_free(XdapDetached *);
#endif // XDAP_DETACHED_H
