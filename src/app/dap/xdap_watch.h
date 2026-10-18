/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_watch.h - Owned watch text and stable session identities
 */
#ifndef XDAP_WATCH_H
#define XDAP_WATCH_H
#include "xdap_dispatch.h"
typedef struct XdapWatchText { uint32_t id;char *bytes;size_t length; } XdapWatchText;
/* Watch IDs belong to this adapter, survive resume/restart and are never reused.
 * Text carries no cached frame, value, write permission or execution authority.
 * Explicit additions are independent; wire watch evaluations intern exact text.
 * All methods belong to the sole control owner and reject callback reentry. */
XR_FUNC XrXirStatus xdap_watch_add(XdapAdapter *,const char *,size_t,uint32_t *);
XR_FUNC XrXirStatus xdap_watch_count(const XdapAdapter *,uint32_t *);
XR_FUNC XrXirStatus xdap_watch_at(const XdapAdapter *,uint32_t,XdapWatchText *);
XR_FUNC XrXirStatus xdap_watch_remove(XdapAdapter *,uint32_t);
XR_FUNC XrXirStatus xdap_watch_clear(XdapAdapter *);
/* Evaluation resolves the selected current stopped frame through the ordinary
 * readonly Source gate. Failure preserves an empty output and every watch. */
XR_FUNC XrXirStatus xdap_watch_evaluate(XdapAdapter *,uint32_t watch,uint32_t frame,XrXirValue *);
/* A copied text pins its original ledger and may outlive every DAP object. */
XR_FUNC void xdap_watch_text_drop(XdapWatchText *);
#endif // XDAP_WATCH_H
