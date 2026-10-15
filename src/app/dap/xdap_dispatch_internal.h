/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_dispatch_internal.h - Owned DAP stop references and message helpers
 */
#ifndef XDAP_DISPATCH_INTERNAL_H
#define XDAP_DISPATCH_INTERNAL_H
#include "xdap_dispatch.h"
#include "../../xir/xxir_output.h"
#include "xdap_value.h"
typedef struct XdapStopFrame { uint32_t handle, scope; } XdapStopFrame;
typedef struct XdapValueRef { uint32_t handle,frame;XrXirValue value;XdapValueShape shape; } XdapValueRef;
typedef struct XdapSourceBreakpoint {
    char *identity;
    size_t identity_length;
    uint32_t line,column;
    XrXirVmDebugRule rule;
} XdapSourceBreakpoint;
typedef struct XdapBreakpointSet {
    char *const *functions;
    const XrXirVmDebugRule *function_rules;
    uint32_t function_count;
    const XdapSourceBreakpoint *sources;
    uint32_t source_count;
} XdapBreakpointSet;
struct XdapAdapter {
    XrSourceCompiler *compiler;
    XrCompileResources *resources;
    XdapAdapterConfig config;
    XrXirOutputSink sink;
    XdapLaunch *launch;
    XrXirVmDebugSnapshot *snapshot;
    XrXirCallResult outcome;
    XdapStopFrame frames[64];
    XdapValueRef values[128];
    uint32_t value_count;
    char *function_breakpoints[32];
    XrXirVmDebugRule function_rules[32];
    uint32_t function_breakpoint_count;
    XdapSourceBreakpoint source_breakpoints[32];
    uint32_t source_breakpoint_count;
    uint32_t depth, next_handle, seq;
    uint64_t stop_epoch;
    uint64_t wait_epoch, wait_wake, wait_start, wait_duration;
    bool initialized, configured, running, closed, busy, terminal, stop_entry, variable_types, variable_paging;
    const char *stop_reason;
    bool break_throw;
};
typedef struct XdapRequest {
    XdapJsonDocument document;
    uint32_t arguments, seq, command;
} XdapRequest;
#define XDAP_TEXT(writer,literal) xdap_json_text((writer),(literal),sizeof(literal)-1)
XR_FUNC XrXirStatus xdap_dispatch_response(XdapAdapter *,const XdapRequest *,bool,
    const XdapJsonWriter *,const char *);
XR_FUNC XrXirStatus xdap_dispatch_prepare_response(XdapAdapter *,const XdapRequest *,bool,
    const XdapJsonWriter *,const char *,XdapJsonWriter *);
XR_FUNC XrXirStatus xdap_dispatch_send_prepared(XdapAdapter *,XdapJsonWriter *);
XR_FUNC XrXirStatus xdap_dispatch_event(XdapAdapter *,const char *,const XdapJsonWriter *);
XR_FUNC XrXirStatus xdap_dispatch_argument(const XdapRequest *,const char *,uint32_t *);
XR_FUNC XrXirStatus xdap_dispatch_number(const XdapRequest *,const char *,uint64_t,uint64_t *);
XR_FUNC XrXirStatus xdap_dispatch_inspect(XdapAdapter *,const XdapRequest *,const char *);
XR_FUNC XrXirStatus xdap_dispatch_exception(XdapAdapter *,const XdapRequest *,bool configure);
XR_FUNC void xdap_dispatch_clear_stop(XdapAdapter *);
XR_FUNC XrXirStatus xdap_breakpoints_collect(const XdapLaunch *,const XdapBreakpointSet *,XrXirVmDebugRule *,uint32_t *);
XR_FUNC XrXirStatus xdap_dispatch_source_breakpoints(XdapAdapter *,const XdapRequest *);
#endif // XDAP_DISPATCH_INTERNAL_H
