/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch.h - Current DAP request lifecycle and execution driving
 */
#ifndef XDAP_DISPATCH_H
#define XDAP_DISPATCH_H
#include "xdap_launch.h"
#include "xdap_json.h"
typedef struct XdapAdapter XdapAdapter;
typedef XrXirStatus (*XdapMessageWrite)(void *,const char *,size_t);
typedef struct XdapAdapterConfig {
    XrXirDomain *observer_metadata;
    XrXirInstanceConfig instance;
    XdapMessageWrite write;
    void *context;
    size_t message_limit;
} XdapAdapterConfig;
/* Success takes compiler; failure preserves both owners. Every method belongs
 * to the sole control/drive owner. Callback reentry cannot mutate the adapter.
 * Write receives one complete unframed JSON message synchronously. */
XR_FUNC XrXirStatus xdap_adapter_new(XrSourceCompiler **,const XdapAdapterConfig *,XdapAdapter **);
XR_FUNC XrXirStatus xdap_adapter_request(XdapAdapter *,const char *,size_t);
XR_FUNC XrXirStatus xdap_adapter_step(XdapAdapter *,uint64_t quantum);
XR_FUNC bool xdap_adapter_running(const XdapAdapter *);
XR_FUNC bool xdap_adapter_closed(const XdapAdapter *);
XR_FUNC XrXirCallStatus xdap_adapter_free(XdapAdapter *);
#endif // XDAP_DISPATCH_H
