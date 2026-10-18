/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_stdio.h - Owned bounded stdio frames for current DAP
 */
#ifndef XDAP_STDIO_H
#define XDAP_STDIO_H
#include "xdap_json.h"
#include "../../xir/xxir_output.h"
typedef struct XdapStdio XdapStdio;
XR_FUNC XrXirStatus xdap_stdio_new(XrCompileResources *,size_t body_limit,XdapStdio **);
XR_FUNC void xdap_stdio_free(XdapStdio *);
/* A complete buffered frame is delivered before EOF. The owned message pins
 * its original ledger; empty reads preserve output. EOF with a partial frame
 * is an IO failure, not a silently discarded successful disconnect. */
XR_FUNC XrXirStatus xdap_stdio_read(XdapStdio *,char **,size_t *,bool *eof);
XR_FUNC XrXirStatus xdap_stdio_write(void *,const char *,size_t);
XR_FUNC XrXirStatus xdap_stdio_wait(XdapStdio *,uint32_t milliseconds);
/* Unframed inherited host stdout/stderr. The context must be NULL and the
 * host must keep its standard streams alive. Use only when the control
 * connection is separate, so detached output cannot become DAP frame bytes. */
XR_FUNC XrXirOutputStatus xdap_stdio_host_output(void *,XrXirOutputStream,const char *,size_t);
#endif // XDAP_STDIO_H
