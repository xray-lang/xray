/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_view_liveness_internal.h - Borrowed SSA demand over actual control flow
 *
 * KEY CONCEPT:
 *   Liveness identifies demanded owners; it grants no root or access proof.
 */
#ifndef XXIR_VIEW_LIVENESS_INTERNAL_H
#define XXIR_VIEW_LIVENESS_INTERNAL_H
#include "xxir.h"

typedef struct XrXirViewLiveSet {
    const uint32_t *values;
    const uint64_t *bits;
    uint32_t count,words;
} XrXirViewLiveSet;
typedef enum XrXirViewLiveKind {
    XR_XIR_VIEW_LIVE_ENTRY,XR_XIR_VIEW_LIVE_INSTRUCTION,XR_XIR_VIEW_LIVE_EDGE
} XrXirViewLiveKind;
typedef struct XrXirViewLiveEvent {
    XrXirViewLiveKind kind;
    uint32_t function,block,instruction,target;
    bool panic;
    XrXirViewLiveSet before,after;
} XrXirViewLiveEvent;
typedef XrXirStatus (*XrXirViewLiveVisitor)(void *user,const XrXirCompileContext *context,
    const XrXirViewLiveEvent *event);

/* Requires complete ordinary type/structure/SSA admission of this immutable
 * module generation. All borrowed descriptors are independently recomputed.
 * Events visit reachable blocks, reverse instruction order and actual edges.
 * PHI operands occur on their own normal predecessor edge; panic demand is
 * retained through every prefix of the protected block. Borrowed arrays live
 * only during the callback; no pointer or permission is published. The visitor
 * must not mutate the module and discards all partial output on later failure.
 * Root aliases, transfers and physical END insertion
 * remain separate obligations even when a value is absent from a live set. */
XR_FUNC XrXirStatus xir_compile_view_liveness_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirViewLiveVisitor visitor,void *user,XrXirDiagnostic *diagnostic);
#endif // XXIR_VIEW_LIVENESS_INTERNAL_H
