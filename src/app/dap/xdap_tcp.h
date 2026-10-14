/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_tcp.h - Owned loopback endpoint for current DAP framing
 */
#ifndef XDAP_TCP_H
#define XDAP_TCP_H
#include "xdap_framed.h"
typedef struct XdapTcp XdapTcp;
/* Accept one local peer. Port zero chooses an ephemeral port. No remote
 * attachment or wildcard listener is granted. Socket and buffers are owned. */
XR_FUNC XrXirStatus xdap_tcp_new(XrCompileResources *,uint32_t port,size_t body_limit,XdapTcp **);
XR_FUNC void xdap_tcp_free(XdapTcp *);
XR_FUNC uint32_t xdap_tcp_port(const XdapTcp *);
XR_FUNC XrXirStatus xdap_tcp_read(XdapTcp *,char **,size_t *,bool *);
XR_FUNC XrXirStatus xdap_tcp_write(void *,const char *,size_t);
XR_FUNC XrXirStatus xdap_tcp_wait(XdapTcp *,uint32_t);
#endif // XDAP_TCP_H
