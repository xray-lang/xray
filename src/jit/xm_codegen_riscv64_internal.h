/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_riscv64_internal.h - Internal types for RISC-V 64 codegen
 *
 * KEY CONCEPT:
 *   Shared state between xm_codegen_riscv64.c and its sub-files.
 *   Mirrors the role of xm_codegen_x64_internal.h for x86-64.
 *
 * CALLING CONVENTION (LP64D — standard RISC-V Linux):
 *   Integer args: a0-a7 (x10-x17)     Return: a0/a1 (x10/x11)
 *   Float args:   fa0-fa7 (f10-f17)   Return: fa0/fa1 (f10/f11)
 *   Caller-saved: t0-t6, a0-a7, ft0-ft11, fa0-fa7
 *   Callee-saved: s0-s11, fs0-fs11, ra
 *
 * JIT REGISTER ASSIGNMENT:
 *   s11 (x27) = coroutine pointer  (callee-saved, reserved)
 *   s10 (x26) = jit_ctx pointer    (callee-saved, reserved)
 *   s0  (x8)  = frame pointer      (callee-saved, reserved)
 *   sp  (x2)  = stack pointer      (reserved)
 *   t6  (x31) = scratch            (caller-saved, not allocatable)
 *   t5  (x30) = scratch2           (caller-saved, not allocatable)
 */

#ifndef XM_CODEGEN_RISCV64_INTERNAL_H
#define XM_CODEGEN_RISCV64_INTERNAL_H

#include <setjmp.h>
#include "xm_codegen.h"
#include "xm_codegen_internal.h"
#include "xm_riscv64.h"
#include "xm_jit.h"
#include "xm_regalloc.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"

/* ========== ABI Constants ========== */

/* LP64D: 8 integer arg registers, 8 FP arg registers */
#define RV64_ABI_ARG1 RV64_A0
#define RV64_ABI_ARG2 RV64_A1
#define RV64_ABI_ARG3 RV64_A2
#define RV64_ABI_ARG4 RV64_A3
#define RV64_ABI_ARG5 RV64_A4
#define RV64_ABI_ARG6 RV64_A5
#define RV64_ABI_ARG7 RV64_A6
#define RV64_ABI_ARG8 RV64_A7

#define RV64_NGPR_CALLER_SAVE 12      /* a0-a7 + t0-t3 */
#define RV64_NGPR_CALLEE_SAVE_ALLOC 8 /* s1 + s2-s8 */
#define RV64_NFPR_CALLER_SAVE 12      /* fa0-fa7 + ft0-ft3 */

/* Frame layout (RISC-V 64):
 *
 *   [higher addresses]
 *     caller's frame
 *     return address           (saved by prologue: sd ra, [sp+X])
 *     saved s0/fp              (saved by prologue: sd s0, [sp+X])
 *     --- callee-saved regs ---
 *     [stack_map_ptr]          8 bytes
 *     [safepoint_id]           8 bytes
 *     --- spill slots ---      ← sp after prologue
 *
 * Callee-saved: ra + s0 + s1-s9 = 11 * 8 = 88B
 * FP callee-saved: fs0-fs7 = 8 * 8 = 64B
 * Stack map metadata: 16B
 * Total frame_base = 88 + 64 + 16 = 168B (rounded to 16-byte alignment = 176B)
 */
#define RV64_FRAME_SMAP_PTR_OFFSET 8 /* [fp - 8]:  XrStackMapTable* */
#define RV64_FRAME_SMAP_ID_OFFSET 16 /* [fp - 16]: safepoint_id */
#define RV64_SPILL_BASE 176
#define RV64_JIT_FRAME_BASE 176

/* Extra-arg scratch: reuse the call_args[15] slot to pass extra_arg to
 * call_c_stub. Safe because call_args are written before the call and
 * the extra_arg is consumed before any call_args are read by the callee. */
#define RV64_EXTRA_ARG_OFFSET XM_JIT_TAG_SCRATCH_OFFSET

/* ========== Branch Patch ========== */

typedef enum {
    RV64_PATCH_JAL,          /* unconditional JAL x0, offset */
    RV64_PATCH_BRANCH,       /* conditional branch (beq/bne/blt/bge/bltu/bgeu) */
    RV64_PATCH_DEOPT_BRANCH, /* deopt: conditional branch to deopt stub */
    RV64_PATCH_DEOPT_JAL,    /* deopt: unconditional jump to deopt stub */
    RV64_PATCH_CALL_C,       /* call_c_stub via JAL */
    RV64_PATCH_CALL_SELF,    /* self-recursive call via JAL */
    RV64_PATCH_BARRIER_FWD,  /* forward barrier stub via JAL */
    RV64_PATCH_BARRIER_BACK, /* back barrier stub via JAL */
} Rv64PatchType;

typedef struct {
    uint32_t emit_idx;   /* instruction index in code buffer */
    uint32_t target_blk; /* target block id */
    Rv64PatchType type;
    Rv64Cond cc; /* condition code (for branch patches) */
    uint8_t rs1; /* first source register (for branch) */
    uint8_t rs2; /* second source register (for branch) */
} Rv64BranchPatch;

#define RV64_INIT_PATCHES 256

/* ========== Codegen Context ========== */

typedef struct {
    XmFunc *func;
    XmCodeAlloc *alloc;
    Rv64Buf buf;

    uint32_t *block_offsets; /* byte offset of each block's start */
    uint32_t nblock_offsets;

    Rv64BranchPatch *patches; /* deferred branch patches */
    uint32_t npatch;
    uint32_t patches_cap;

    XraResult *xra;        /* register allocation result */
    int8_t *vreg_override; /* gap-move overrides (-128 = no override) */

    uint32_t cur_blk_id;
    int32_t cur_ra_pos;
    uint32_t cur_ins_idx;
    uint32_t gap_move_cursor;

    uint32_t fast_entry_offset;

    /* Frame size patch locations */
    uint32_t frame_patch_sub[16];
    uint32_t frame_patch_add[8];
    uint32_t nsub_patches;
    uint32_t nadd_patches;

    OsrSnapshot osr_snaps[XM_MAX_OSR_ENTRIES];
    uint32_t nosr_snap;

    uint32_t call_c_stub;
    uint32_t deopt_stub;
    uint32_t barrier_fwd_stub;
    uint32_t barrier_back_stub;

    /* GC stack map */
    XrStackMapEntry smap_entries[XM_MAX_STACK_MAP_ENTRIES];
    uint32_t nsmap;

    bool had_error;
    const char *error_reason;
    jmp_buf bail_jmp;

    bool has_deopt;
    bool has_call_c;
    bool has_barriers;

    /* Suspend/resume tracking */
    uint32_t suspend_cont_offsets[16];
    uint32_t suspend_smap_ids[16];
    uint8_t suspend_result_regs[16];
    int16_t suspend_result_bc_slots[16];
    int32_t suspend_result_tag_offs[16];
    uint32_t nsuspend;
    uint32_t resume_entry_offset;
} Rv64CodegenCtx;

/* ========== Register Mapping ========== */

/* Get the RISC-V GP hardware register assigned to a vreg reference */
XR_FUNC Rv64Reg rv64_get_reg(Rv64CodegenCtx *ctx, XmRef ref);

/* Get GP register for a source operand, loading from spill slot if needed */
XR_FUNC Rv64Reg rv64_get_operand(Rv64CodegenCtx *ctx, XmRef ref, Rv64Reg scratch);

/* Get the FP register assigned to a vreg reference */
XR_FUNC Rv64Freg rv64_get_fp_reg(Rv64CodegenCtx *ctx, XmRef ref);

/* Get FP register for a source operand, loading constant or spill if needed */
XR_FUNC Rv64Freg rv64_get_fp_operand(Rv64CodegenCtx *ctx, XmRef ref, Rv64Freg scratch);

/* Load a 64-bit constant into a register using optimal encoding */
XR_FUNC void rv64_load_imm64(Rv64Buf *buf, Rv64Reg dst, uint64_t val);

/* Store to spill slot if vreg has one assigned (GP or FP) */
XR_FUNC void rv64_maybe_spill(Rv64CodegenCtx *ctx, XmRef dst_ref);

/* Add a deferred branch patch */
XR_FUNC void rv64_add_patch(Rv64CodegenCtx *ctx, Rv64PatchType type, uint32_t target_blk,
                            Rv64Cond cc, uint8_t rs1, uint8_t rs2);

/* Check whether a vreg holds a float-rep value */
static inline bool rv64_is_fp_vreg(Rv64CodegenCtx *ctx, XmRef ref) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= ctx->func->nvreg)
        return false;
    return ctx->func->vregs[idx].rep == XR_REP_F64;
}

/* Derive XR_TAG_* from const rep for call_arg_tags[] */
static inline uint8_t rv64_const_rep_to_value_tag(uint8_t rep) {
    switch (rep) {
        case XR_REP_I64:
            return 3; /* XR_TAG_I64 */
        case XR_REP_F64:
            return 4; /* XR_TAG_F64 */
        case XR_REP_PTR:
            return 5; /* XR_TAG_PTR */
        default:
            return 0xFF; /* XR_RTAG_UNKNOWN */
    }
}

/* ========== Sub-emit functions ========== */

/* Per-instruction emission (xm_codegen_riscv64_ins.c) */
XR_FUNC void rv64_emit_xm_ins(Rv64CodegenCtx *ctx, XmIns *ins);

/* Epilogue emission (xm_codegen_riscv64.c) — shared with deopt stub */
XR_FUNC void rv64_emit_epilogue(Rv64CodegenCtx *ctx);

/* Stub emission (xm_codegen_riscv64_stub.c) */
XR_FUNC void rv64_emit_call_c_stub(Rv64CodegenCtx *ctx);
XR_FUNC void rv64_emit_deopt_stub(Rv64CodegenCtx *ctx);
XR_FUNC void rv64_emit_barrier_stubs(Rv64CodegenCtx *ctx);
XR_FUNC void rv64_emit_deopt_branch(Rv64CodegenCtx *ctx, Rv64Reg cond_reg);
XR_FUNC void rv64_emit_deopt_jmp(Rv64CodegenCtx *ctx);
XR_FUNC void rv64_emit_deopt_id(Rv64CodegenCtx *ctx, XmIns *ins);
XR_FUNC uint32_t rv64_record_safepoint(Rv64CodegenCtx *ctx);
XR_FUNC void rv64_emit_ptr_spill_writeback(Rv64CodegenCtx *ctx);
XR_FUNC int rv64_live_gp(Rv64CodegenCtx *ctx, Rv64Reg *out, Rv64Reg exclude);
XR_FUNC int rv64_live_fp(Rv64CodegenCtx *ctx, Rv64Freg *out);

/* Call instruction emission (xm_codegen_riscv64_call.c) */
XR_FUNC bool rv64_emit_call_ins(Rv64CodegenCtx *ctx, XmIns *ins, Rv64Reg rd);
XR_FUNC void rv64_emit_call_args_from_pool(Rv64CodegenCtx *ctx, XmIns *ins);

/* Bail out of codegen on invariant violation */
#define RV64_CODEGEN_CHECK(ctx, cond, msg)                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (ctx)->had_error = true;                                                               \
            (ctx)->error_reason = (msg);                                                           \
            xr_log_warning("rv64-cg", "bail: %s (%s:%d)", (msg), __FILE__, __LINE__);              \
            longjmp((ctx)->bail_jmp, 1);                                                           \
        }                                                                                          \
    } while (0)

#endif  // XM_CODEGEN_RISCV64_INTERNAL_H
