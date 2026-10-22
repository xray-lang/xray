/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_rebuild_internal.h - Bounded immutable instruction projection
 *
 * KEY CONCEPT:
 *   Identity copying and SCCP share ownership and mapping, not mutable SSA.
 */
#ifndef XLIR_REBUILD_INTERNAL_H
#define XLIR_REBUILD_INTERNAL_H
#include "xlir.h"

/* begin owns analysis scratch; end runs once even when begin fails. The other
 * callbacks cannot allocate or fail. Rewrites modify only the local view,
 * preserve result/type/source, and cannot grow its operand/edge vectors.
 * Borrowed const arrays remain untouched. This does not describe inlining. */
typedef struct XrLirCopyPlan {
    void *context;
    XrLirStatus (*begin)(void *, const XrLirModule *, XrLirFunction);
    bool (*keep_block)(void *, uint32_t);
    void (*instruction)(void *, uint32_t, uint32_t, XrLirInstructionView *);
    void (*end)(void *);
} XrLirCopyPlan;

XR_FUNC XrLirStatus xr_lir_internal_copy(const XrLirModule *, XrLirModule **,
    const XrLirCopyPlan *);
#endif // XLIR_REBUILD_INTERNAL_H
