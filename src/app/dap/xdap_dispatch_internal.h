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
#include "xdap_watch.h"
#include "../../xir/xxir_output.h"
#include "xdap_value.h"
typedef struct XdapStopFrame { uint32_t handle, scope,global_scope; } XdapStopFrame;
/* A Source operand or module slot and path select a value, never a cached Call permission.
 * A readonly value root can reach a mutable Class identity but cannot rebind. */
typedef struct XdapValueOrigin { uint64_t epoch,frame;uint32_t operand;XrXirValuePath path;bool slot; } XdapValueOrigin;
typedef struct XdapValueRef { uint32_t handle,frame;XrXirValue value;XdapValueShape shape;XdapValueOrigin origin; } XdapValueRef;
typedef struct XdapBreakpointPredicate {
    char *condition,*log;
    size_t condition_length,log_length;
    bool logpoint;
} XdapBreakpointPredicate;
typedef struct XdapSourceBreakpoint {
    char *identity;
    size_t identity_length;
    uint32_t line,column;
    XrXirVmDebugRule rule;
    XdapBreakpointPredicate predicate;
} XdapSourceBreakpoint;
typedef struct XdapBreakpointSet {
    char *const *functions;
    const XrXirVmDebugRule *function_rules;
    uint32_t function_count;
    const XdapSourceBreakpoint *sources;
    uint32_t source_count;
} XdapBreakpointSet;
struct XdapDetached {
    XrCompileResources *resources;
    XdapAdapter *adapter;
    XdapLaunch *launch;
    XrXirOutputProvider output;
    XrXirTimeProvider time;
    XrXirCallResult outcome;
    uint64_t wait_epoch,wait_wake,wait_start,wait_duration;
    bool started,ended,driving,retain_output;
};
struct XdapAdapter {
    XrSourceCompiler *compiler;
    XrCompileResources *resources;
    XdapAdapterConfig config;
    XrXirOutputSink sink;
    XdapLaunch *launch;
    XdapDetached *execution;
    XrXirVmDebugSnapshot *snapshot;
    XrXirCallResult outcome;
    XdapStopFrame frames[64];
    XdapValueRef values[128];
    uint32_t value_count;
    XdapWatchText watches[128];
    uint32_t watch_count,next_watch_id;
    size_t watch_bytes;
    char *function_breakpoints[32];
    XrXirVmDebugRule function_rules[32];
    XdapBreakpointPredicate function_predicates[32];
    uint32_t function_breakpoint_count;
    XdapSourceBreakpoint source_breakpoints[32];
    uint32_t source_breakpoint_count;
    uint32_t depth, next_handle, seq;
    uint64_t stop_epoch;
    uint64_t wait_epoch, wait_wake, wait_start, wait_duration;
    bool initialized, configured, running, closed, busy, terminal, stop_entry, variable_types, variable_paging, attached;
    const char *stop_reason;
    uint32_t break_throw;
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
/* Reserve the widest sequence before target mutation. Sending rewrites only
 * that prepaid numeric field, without allocation or additional ledger work. */
XR_FUNC XrXirStatus xdap_dispatch_stage_response(XdapAdapter *,const XdapRequest *,bool,const XdapJsonWriter *,const char *,XdapJsonWriter *);
XR_FUNC XrXirStatus xdap_dispatch_send_staged(XdapAdapter *,XdapJsonWriter *);
XR_FUNC XrXirStatus xdap_dispatch_event(XdapAdapter *,const char *,const XdapJsonWriter *);
XR_FUNC XrXirStatus xdap_dispatch_argument(const XdapRequest *,const char *,uint32_t *);
XR_FUNC XrXirStatus xdap_dispatch_number(const XdapRequest *,const char *,uint64_t,uint64_t *);
XR_FUNC XrXirStatus xdap_dispatch_inspect(XdapAdapter *,const XdapRequest *,const char *);
XR_FUNC XrXirStatus xdap_dispatch_exception(XdapAdapter *,const XdapRequest *,bool configure);
XR_FUNC void xdap_dispatch_clear_stop(XdapAdapter *);
/* Destructor cleanup does not need another work admission. */
XR_FUNC void xdap_watches_dispose(XdapAdapter *);
XR_FUNC void xdap_value_ref_drop(XdapValueRef *);
XR_FUNC XrXirStatus xdap_breakpoints_collect(const XdapLaunch *,const XdapBreakpointSet *,XrXirVmDebugRule *,uint32_t *);
XR_FUNC XrXirStatus xdap_dispatch_source_breakpoints(XdapAdapter *,const XdapRequest *);
XR_FUNC XrXirStatus xdap_dispatch_function_breakpoints(XdapAdapter *,const XdapRequest *);
XR_FUNC XrXirStatus xdap_predicate_prepare(XdapAdapter *,const XdapRequest *,uint32_t,uint32_t,XdapBreakpointPredicate *);
XR_FUNC void xdap_predicate_free(XdapBreakpointPredicate *);
XR_FUNC XrXirStatus xdap_predicates_probe(XdapAdapter *,bool *);
#endif // XDAP_DISPATCH_INTERNAL_H
