/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_may_owned_internal.h - Possible borrowed owners over actual CFGs
 *
 * KEY CONCEPT:
 *   A possible physical owner is neither a logical use grant nor a loan.
 */
#ifndef XXIR_VIEW_MAY_OWNED_INTERNAL_H
#define XXIR_VIEW_MAY_OWNED_INTERNAL_H
#include "xxir.h"

typedef struct XrXirViewOwnerSet {
    const uint32_t *values;
    const uint64_t *bits;
    uint32_t count,words;
} XrXirViewOwnerSet;
typedef enum XrXirViewOwnerKind {
    XR_XIR_VIEW_OWNER_ENTRY,XR_XIR_VIEW_OWNER_INSTRUCTION,XR_XIR_VIEW_OWNER_EDGE
} XrXirViewOwnerKind;
typedef struct XrXirViewOwnerEvent {
    XrXirViewOwnerKind kind;
    uint32_t function,block,instruction,target;
    bool panic;
    XrXirViewOwnerSet before,after;
} XrXirViewOwnerEvent;
typedef XrXirStatus (*XrXirViewOwnerVisitor)(void *user,const XrXirCompileContext *context,
    const XrXirViewOwnerEvent *event);

/* Requires complete ordinary and independent affine admission of the same
 * immutable module generation. Inputs meet by union; explicit END/CLEANUP
 * and Mut transfers kill owners, while Shared copies retain their sources.
 * Normal edges apply simultaneous Mut PHI source transfers. A panic edge
 * includes every protected block prefix, including local definitions that
 * do not dominate its handler. Its before set is the normal block exit;
 * its after set is the union of possible prefix states, without PHI inputs.
 * Events visit only reachable blocks in forward instruction order. Arrays
 * expire on callback return; discard all partial observations on failure.
 * None wrappers may own an empty physical slot. No liveness, root lifetime,
 * logical availability, alias, reborrow or access permission is issued. */
XR_FUNC XrXirStatus xir_compile_view_may_owned_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirViewOwnerVisitor visitor,void *user,XrXirDiagnostic *diagnostic);
/* Replays one function with the same independent analysis. The callback's
 * borrowed context reports remaining frame space after all live analysis
 * storage, so a receiving analysis can compose its actual scratch budget.
 * The context itself expires with the callback; resources retain their owner. */
XR_FUNC XrXirStatus xir_compile_view_may_owned_function_scoped(const XrXirCompileContext *context,
    const XrXirModule *module,uint32_t function,XrXirViewOwnerVisitor visitor,void *user,XrXirDiagnostic *diagnostic);
#endif // XXIR_VIEW_MAY_OWNED_INTERNAL_H
