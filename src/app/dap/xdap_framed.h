/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_framed.h - Bounded owned DAP framing over a borrowed host byte stream
 */
#ifndef XDAP_FRAMED_H
#define XDAP_FRAMED_H
#include "xdap_json.h"
typedef struct XdapFramed XdapFramed;
typedef struct XdapByteStream {
    void *context;
    XrXirStatus (*read)(void *,char *,size_t,size_t *,bool *);
    XrXirStatus (*write)(void *,const char *,size_t);
    XrXirStatus (*wait)(void *,uint32_t);
} XdapByteStream;
/* The provider remains alive until free and must exclude reentry. Its read
 * writes at most capacity bytes; its write either sends every byte or fails.
 * Framing owns only its buffers on the original cumulative compiler ledger. */
XR_FUNC XrXirStatus xdap_framed_new(XrCompileResources *,size_t,const XdapByteStream *,XdapFramed **);
XR_FUNC void xdap_framed_free(XdapFramed *);
XR_FUNC XrXirStatus xdap_framed_read(XdapFramed *,char **,size_t *,bool *);
XR_FUNC XrXirStatus xdap_framed_write(void *,const char *,size_t);
XR_FUNC XrXirStatus xdap_framed_wait(XdapFramed *,uint32_t);
#endif // XDAP_FRAMED_H
