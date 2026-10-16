/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_comptime_instances_internal.h - Actual typed comptime call results
 *
 * KEY CONCEPT:
 *   The ordinary Source producer and specializer discover authentic call sites.
 *   Execution results retain those coordinates without publishing runtime code.
 */
#ifndef XXIR_SOURCE_COMPTIME_INSTANCES_INTERNAL_H
#define XXIR_SOURCE_COMPTIME_INSTANCES_INTERNAL_H
#include "xxir_source_instance_internal.h"
typedef struct XirSourceComptimeOwner XirSourceComptimeOwner;
typedef struct XirSourceComptimeBinding {
    uint32_t source_function,source_instruction,helper;
    uint32_t function,instruction,callee;
} XirSourceComptimeBinding;
typedef struct XirSourceComptimeRequest {
    const XrXirSourceRequest *source;
    const XrXirSourceText *text;
    const XrModuleOverlayInput *overlays;
    size_t overlay_count;
    XrXirTarget target;
    XrXirCtfeLimits limits;
    uint32_t call_depth;
} XirSourceComptimeRequest;
typedef struct XirSourceComptimeInstances { XirSourceComptimeOwner *owner;uint32_t count; } XirSourceComptimeInstances;
/* Discover every collected generic comptime call in the actual Program graph,
 * independently receive Checked/Lowered and freshly evaluate each real helper.
 * No caller-selected function, type ID, Value or admission hint enters. Input
 * remains immutable throughout the call. This result is compiler staging data;
 * it does not publish Source code or resolve language expression replacement. */
XR_FUNC XrXirStatus xr_xir_compile_source_comptime_instances(const XirSourceComptimeRequest *request,
    XirSourceComptimeInstances *output,XrXirSourceDiagnostic *diagnostic);
/* All count caller-owned view/binding slots are zero. Owner remains alive and
 * immutable for the borrow. Every query freshly verifies current artifacts,
 * exact source packets and actual call coordinates before atomic publication. */
XR_FUNC XrXirCtfeStatus xr_xir_compile_source_comptime_instances_view(const XirSourceComptimeInstances *owner,
    XirCtfeInstanceView *views,XirSourceComptimeBinding *bindings,uint32_t count);
XR_FUNC void xr_xir_compile_source_comptime_instances_free(XirSourceComptimeInstances *owner);
#endif // XXIR_SOURCE_COMPTIME_INSTANCES_INTERNAL_H
