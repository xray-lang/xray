/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_roots_internal.h - Actual root and projection source observations
 *
 * KEY CONCEPT:
 *   Producer paths identify possible roots, never lifetime or access grants.
 */
#ifndef XXIR_VIEW_ROOTS_INTERNAL_H
#define XXIR_VIEW_ROOTS_INTERNAL_H
#include "xxir.h"
typedef enum XrXirViewRootKind {
    XR_XIR_VIEW_ROOT_CELL,XR_XIR_VIEW_ROOT_PARAMETER,XR_XIR_VIEW_ROOT_MODULE,
    XR_XIR_VIEW_ROOT_OBJECT,XR_XIR_VIEW_ROOT_BORROWED_PARAMETER,
    XR_XIR_VIEW_ROOT_ABSENT,XR_XIR_VIEW_ROOT_UNRESOLVED
} XrXirViewRootKind;
typedef struct XrXirViewRootStep {
    uint32_t function,value;
    XrXirOp operation;
    XrXirType container;
    uint32_t selector;
} XrXirViewRootStep;
typedef struct XrXirViewRootEvent {
    uint32_t function,instruction,root_function,root_value;
    XrXirViewRootKind kind;
    const XrXirViewRootStep *steps;
    uint32_t step_count;
    const uint32_t *selection;
    uint32_t selection_count;
    bool opaque_suffix,dynamic_identity;
} XrXirViewRootEvent;
typedef XrXirStatus (*XrXirViewRootVisitor)(void *user,const XrXirCompileContext *context,
    const XrXirViewRootEvent *event);
/* Requires ordinary type, structure and SSA admission of the immutable module.
 * Replays actual producers, known call bindings and ordinary Cell captures.
 * Cell roots that may be shared captures fail closed. Object identity and
 * unknown call suffixes remain explicit; they grant no disjointness or access.
 * FIELD selectors are field ordinals. INDEX selectors are SSA values in their
 * named function, including exact Map keys. Selection uses tuple ordinals and
 * UINT32_MAX for Optional. Arrays expire on return from the visitor; discard
 * every partial observation on later failure. No Source or runtime permission,
 * root lifetime, NLL, physical END or arbitrary indirect target proof is issued. */
XR_FUNC XrXirStatus xir_compile_view_roots_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirViewRootVisitor visitor,void *user,XrXirDiagnostic *diagnostic);

typedef struct XirViewRootResolver XirViewRootResolver;
typedef struct XirViewRootRequest {
    uint32_t function,value,instruction;
    const uint32_t *selection;
    uint32_t selection_count;
} XirViewRootRequest;
/* The module generation and compile context outlive this borrowed owner.
 * The module has complete ordinary type, structure and SSA admission.
 * Parameter roots stay symbolic in their function; actual shared captures
 * are still independently replayed. Module Cell loads follow every actual
 * initializer producer, including grouped initialization. No saved
 * observation, source bit or caller-supplied root grants access permission. */
XR_FUNC XrXirStatus xir_compile_view_root_resolver_new(const XrXirCompileContext *context,
    const XrXirModule *module,XirViewRootResolver **output,XrXirDiagnostic *diagnostic);
XR_FUNC void xir_compile_view_root_resolver_free(XirViewRootResolver *resolver);
XR_FUNC XrXirStatus xir_compile_view_root_resolver_query(XirViewRootResolver *resolver,
    const XirViewRootRequest *request,XrXirViewRootVisitor visitor,void *user,XrXirDiagnostic *diagnostic);
/* Recomputes both requests from actual producers. False includes unresolved
 * overlap; true is only a storage-path fact, never a loan, lifetime, unique
 * object, parameter-disjointness or logical availability grant. All partial
 * results are discarded on failure. Requests name the same function. */
XR_FUNC XrXirStatus xir_compile_view_roots_disjoint(XirViewRootResolver *resolver,
    const XirViewRootRequest *left,const XirViewRootRequest *right,bool *output,XrXirDiagnostic *diagnostic);
#endif // XXIR_VIEW_ROOTS_INTERNAL_H
