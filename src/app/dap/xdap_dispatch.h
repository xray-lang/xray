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
#include "xdap_detached.h"
#include "xdap_current_control.h"
#include "xdap_json.h"
typedef struct XdapAdapter XdapAdapter;
typedef XrXirStatus (*XdapMessageWrite)(void *,const char *,size_t);
typedef struct XdapAdapterConfig {
    XrXirDomain *observer_metadata;
    XrXirInstanceConfig instance;
    XdapMessageWrite write;
    void *context;
    size_t message_limit;
    /* Optional empty host-owned receiving slot. The original instance.output
     * and time contexts must outlive the detached target and every DAP object.
     * Transfer is committed before response I/O; the slot owns on I/O failure. */
    XdapDetached **detached;
    XdapDetachCommit detach_commit;
    void *detach_context;
    /* Mirror each original output group to the retained provider before DAP
     * rendering. Borrowed paths and provider contexts outlive detached cleanup. */
    bool retain_output;
    const char *stdout_path,*stderr_path;
    uint64_t process_id;
} XdapAdapterConfig;
/* Success takes compiler; failure preserves both owners. Every method belongs
 * to the sole control/drive owner. Callback reentry cannot mutate the adapter.
 * Write receives one complete unframed JSON message synchronously. */
XR_FUNC XrXirStatus xdap_adapter_new(XrSourceCompiler **,const XdapAdapterConfig *,XdapAdapter **);
XR_FUNC XrXirStatus xdap_adapter_request(XdapAdapter *,const char *,size_t);
XR_FUNC XrXirStatus xdap_adapter_step(XdapAdapter *,uint64_t quantum);
XR_FUNC bool xdap_adapter_running(const XdapAdapter *);
/* A pending timer remains responsive to requests and consumes no VM polls. */
XR_FUNC bool xdap_adapter_waiting(const XdapAdapter *);
XR_FUNC bool xdap_adapter_closed(const XdapAdapter *);
XR_FUNC XrXirCallStatus xdap_adapter_free(XdapAdapter *);
#endif // XDAP_DISPATCH_H
