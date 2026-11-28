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

typedef struct XrLirCopyEmitter XrLirCopyEmitter;
/* begin owns analysis scratch; end runs once even when begin fails. keep_block
 * and instruction cannot allocate or fail. Rewrites modify only the local view,
 * preserve result/type/source, and cannot grow its operand/edge vectors.
 * Setting op to XR_LIR_OP_INVALID omits a non-terminator; an omitted result
 * then shares the mapping of its single operand, which must have its type, or
 * has no receiver when no operand remains, so any later use is refused.
 * Borrowed const arrays remain untouched. This does not describe inlining.
 * The optional around callback runs before each copied instruction and after
 * each non-terminator and may emit instructions through the emitter; the
 * optional held callback marks a copied Array block parameter as held. */
typedef struct XrLirCopyPlan {
    void *context;
    XrLirStatus (*begin)(void *, const XrLirModule *, XrLirFunction);
    bool (*keep_block)(void *, uint32_t);
    void (*instruction)(void *, uint32_t, uint32_t, XrLirInstructionView *);
    void (*end)(void *);
    XrLirStatus (*around)(void *, XrLirCopyEmitter *, uint32_t block, uint32_t ordinal, bool after);
    bool (*held)(void *, uint32_t block, uint32_t parameter);
} XrLirCopyPlan;

/* Emits one non-terminal instruction into the receiving block. Operands are
 * source values mapped at this point; a nonzero bind is the source value the
 * new result now stands for, so later copied instructions read it. */
XR_FUNC XrLirStatus xr_lir_copy_emit(XrLirCopyEmitter *, XrLirOp, XrLirType, uint64_t bits,
    const uint32_t *operands, uint32_t count, uint32_t bind);

XR_FUNC XrLirStatus xr_lir_internal_copy(const XrLirModule *, XrLirModule **,
    const XrLirCopyPlan *);
#endif // XLIR_REBUILD_INTERNAL_H
