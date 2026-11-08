/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_execution_internal.h - Borrowed numeric execution and capture relations
 *
 * KEY CONCEPT:
 *   The shared semantic owner produces actual candidates once. Storage and
 *   lifetime consumers read numeric relations without retaining body storage.
 */
#ifndef XXIR_EXECUTION_INTERNAL_H
#define XXIR_EXECUTION_INTERNAL_H
#include "xxir.h"

typedef struct EffectCheckedRoots XirExecutionOwner;
typedef enum XirExecutionSiteKind {
    XIR_EXECUTION_CALL, XIR_EXECUTION_PENDING, XIR_EXECUTION_CLEANUP_USE
} XirExecutionSiteKind;

typedef enum XirExecutionPort {
    XIR_EXECUTION_NORMAL, XIR_EXECUTION_ERROR, XIR_EXECUTION_PANIC
} XirExecutionPort;

typedef struct XirExecutionSite {
    XirExecutionSiteKind kind;
    uint32_t index;
    uint32_t function, instruction, block, order, caller_instance;
    uint32_t target, declaration;
    uint32_t argument_count, capture_count;
    uint32_t output_function, output_value;
    uint32_t native_callable, environment;
    XirExecutionPort port;
    XrXirType signature;
    uint8_t protocol;
    bool unresolved;
} XirExecutionSite;

typedef struct XirExecutionArgument {
    uint32_t function, value, caller_handle, environment;
    uint32_t mode;
    XrXirType type;
    bool cell, escaping;
} XirExecutionArgument;

typedef XrXirStatus (*XirExecutionVisitor)(void *, const XirExecutionSite *);

/* Function and value ordinals use the physical declaration namespace. Target
 * retains the numeric generic instance identity, while declaration names the
 * admitted body. A site's kind/index stays stable while new edges are linked.
 * Known edges and an unresolved remainder may coexist at one real callsite.
 * Cleanup uses have increasing order in actual LIFO execution order. Panic
 * sequences may name a whole block with instruction UINT32_MAX; their input
 * is the join of the block's before-instruction states. */
XR_FUNC XrXirStatus xir_execution_visit_calls(const XirExecutionOwner *,
    XirExecutionVisitor, void *);
XR_FUNC XrXirStatus xir_execution_visit_captures(const XirExecutionOwner *,
    XirExecutionVisitor, void *);

/* Actuals include the authenticated physical capture prefix. A numeric aux
 * or an unavailable receiver returns UNRESOLVED, never a forged physical SSA
 * ordinal. Caller_handle is UINT32_MAX unless the captured identity has a
 * caller-local physical representative. Foreign values keep their explicit
 * environment application instead. Invalid requests preserve the output. */
XR_FUNC XrXirStatus xir_execution_argument(const XirExecutionOwner *,
    const XirExecutionSite *, uint32_t, XirExecutionArgument *);

/* This reads the same candidate relation as execution linking. Complete is
 * false when any callable origin remains opaque or symbolic. The visitor may
 * receive known bindings even when another origin is unresolved. */
XR_FUNC XrXirStatus xir_execution_visit_value_captures(const XirExecutionOwner *,
    uint32_t, uint32_t, XirExecutionVisitor, void *, bool *complete);

/* Type IDs belong to this owner's stable numeric type namespace. The returned
 * pool is borrowed only during the synchronous query and must be rebound after
 * linking can grow its storage. Native or unresolved calls conservatively
 * clobber reachable writable Cell actuals and callable environments. */
XR_FUNC const XrXirTypes *xir_execution_types(const XirExecutionOwner *);
#endif // XXIR_EXECUTION_INTERNAL_H
