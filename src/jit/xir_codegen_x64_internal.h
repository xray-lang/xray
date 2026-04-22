/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_codegen_x64_internal.h - Internal types for x86-64 codegen
 *
 * KEY CONCEPT:
 *   Shared state between xir_codegen_x64.c and its sub-files.
 *   Mirrors the role of xir_codegen_internal.h for ARM64.
 */

#ifndef XIR_CODEGEN_X64_INTERNAL_H
#define XIR_CODEGEN_X64_INTERNAL_H

#include "xir_codegen.h"
#include "xir_x64.h"
#include "xir_jit.h"
#include "xir_regalloc.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"

/* ========== Constants ========== */

#define X64_MAX_PHYS_REGS   11  // allocatable GP registers (see xir_target_x64.c)
#define X64_MAX_FP_REGS     16
#define X64_MAX_VREGS       4096
#define X64_SCRATCH_REG     X64_R11
#define X64_SCRATCH_XMM     15       // xmm15 as FP scratch
#define X64_CORO_REG        X64_R15
#define X64_JIT_CTX_REG     X64_R14  // jit_ctx pointer (XrJitScratch*)
#define X64_SPILL_BASE      64   // must match xir_target_x64.c
#define X64_JIT_FRAME_BASE  64

/* ========== Branch Patch ========== */

/* Extra-arg scratch: reuse call_args[15] slot to pass extra_arg to call_c_stub.
 * This slot is safe because call_args are written before the call and the
 * extra_arg is consumed before any call_args are read by the callee. */
#define X64_EXTRA_ARG_OFFSET   XIR_JIT_LOAD_TAG_SCRATCH

typedef enum {
    X64_PATCH_JMP,           // unconditional JMP rel32
    X64_PATCH_JCC,           // conditional Jcc rel32
    X64_PATCH_DEOPT_JCC,     // deopt: conditional Jcc to deopt stub
    X64_PATCH_DEOPT_JMP,     // deopt: unconditional JMP rel32 to deopt stub
    X64_PATCH_CALL_C,        // CALL rel32 to shared call_c_stub
    X64_PATCH_CALL_SELF,     // CALL rel32 to function entry (offset 0)
    X64_PATCH_CALL_SELF_FAST,// CALL rel32 to fast_entry_offset
} X64PatchType;

typedef struct {
    uint32_t    emit_pos;    // byte offset of the rel32 field in code buffer
    uint32_t    target_blk;  // target block id
    X64PatchType type;
    X64Cond     cc;          // condition code (for JCC patches)
} X64BranchPatch;

#define X64_INIT_PATCHES 256

/* ========== Codegen Context ========== */

typedef struct {
    XirFunc      *func;
    XirCodeAlloc *alloc;
    X64Buf        buf;

    uint32_t     *block_offsets;    // byte offset of each block's start
    uint32_t      nblock_offsets;

    X64BranchPatch *patches;       // deferred branch patches
    uint32_t      npatch;
    uint32_t      patches_cap;

    XraResult    *xra;             // register allocation result
    int8_t       *vreg_override;   // gap-move overrides (-128 = no override)

    uint32_t      cur_blk_id;
    int32_t       cur_ra_pos;
    uint32_t      cur_ins_idx;
    uint32_t      gap_move_cursor;

    uint32_t      fast_entry_offset;  // byte offset of fast-path entry

    /* Frame size patch locations (byte offsets where sub rsp/add rsp imm32 lives) */
    uint32_t      frame_patch_sub[8];
    uint32_t      frame_patch_add[8];
    uint32_t      nsub_patches;
    uint32_t      nadd_patches;

    uint32_t      call_c_stub;       // byte offset of call_c_stub in code buffer
    uint32_t      deopt_stub;        // byte offset of deopt stub

    /* GC stack map: collect safepoint bitmaps during codegen */
    XrStackMapEntry smap_entries[XIR_MAX_STACK_MAP_ENTRIES];
    uint32_t      nsmap;

    bool          had_error;
    bool          has_deopt;
    bool          has_call_c;

    /* Suspend/resume tracking (coroutine support) */
    uint32_t      suspend_cont_offsets[16];   // byte offset of continuation per suspend_id
    uint32_t      suspend_smap_ids[16];       // smap id at each suspend point
    uint8_t       suspend_result_regs[16];    // physical register for result per suspend_id
    int16_t       suspend_result_bc_slots[16];// bc_slot of result vreg per suspend_id
    uint32_t      nsuspend;                   // number of suspend points emitted
    uint32_t      resume_entry_offset;        // byte offset of resume entry (0 = none)
} X64CodegenCtx;

/* ========== Register Mapping ========== */

/* Map physical register index (0..12) to x86-64 hardware register */
extern const X64Reg x64_alloc_regs[X64_MAX_PHYS_REGS];
extern const X64Reg x64_alloc_fp_regs[X64_MAX_FP_REGS];

/* Get the x86-64 GP hardware register assigned to a vreg reference */
XR_FUNC X64Reg x64_get_reg(X64CodegenCtx *ctx, XirRef ref);

/* Get GP register for a source operand, loading from spill slot if needed */
XR_FUNC X64Reg x64_get_operand(X64CodegenCtx *ctx, XirRef ref, X64Reg scratch);

/* Get the FP (xmm) register assigned to a vreg reference */
XR_FUNC X64Xmm x64_get_fp_reg(X64CodegenCtx *ctx, XirRef ref);

/* Get FP register for a source operand, loading constant or spill if needed */
XR_FUNC X64Xmm x64_get_fp_operand(X64CodegenCtx *ctx, XirRef ref, X64Xmm scratch);

/* Load a 64-bit constant into a register using optimal encoding */
XR_FUNC void x64_load_imm64(X64Buf *buf, X64Reg dst, uint64_t val);

/* Store to spill slot if vreg has one assigned (GP or FP) */
XR_FUNC void x64_maybe_spill(X64CodegenCtx *ctx, XirRef dst_ref);

/* Add a deferred branch patch */
XR_FUNC void x64_add_patch(X64CodegenCtx *ctx, X64PatchType type,
                            uint32_t target_blk, X64Cond cc);

/* ========== Shared helpers (defined in xir_codegen_x64.c) ========== */

/* Write call arguments from pool to jit_ctx->call_args[] */
XR_FUNC void x64_emit_call_args_from_pool(X64CodegenCtx *ctx, XirIns *ins);

/* Write live PTR-typed registers back to their spill slots for GC visibility */
XR_FUNC void x64_emit_ptr_spill_writeback(X64CodegenCtx *ctx);

/* Record a safepoint GC stack map bitmap, return the smap_id */
XR_FUNC uint32_t x64_record_safepoint(X64CodegenCtx *ctx);

/* Collect live caller-saved GP registers (exclude one), return count */
XR_FUNC int x64_live_gp(X64CodegenCtx *ctx, X64Reg *out, X64Reg exclude);

/* Collect live FP registers, return count */
XR_FUNC int x64_live_fp(X64CodegenCtx *ctx, X64Xmm *out);

/* Emit deopt_id store (for deopt stub identification) */
XR_FUNC void x64_emit_deopt_id(X64CodegenCtx *ctx, XirIns *ins);

/* Emit conditional deopt jump (Jcc to deopt stub) */
XR_FUNC void x64_emit_deopt_jcc(X64CodegenCtx *ctx, X64Cond cc);

/* Emit epilogue sequence (restore callee-saved regs, leave, ret) */
XR_FUNC void x64_emit_epilogue(X64CodegenCtx *ctx);

/* ========== Sub-emit functions (defined in split files) ========== */

/* Call ops: CALL_C, CALL_C_LEAF, CALL_SELF_DIRECT, CALL_KNOWN, etc. */
XR_FUNC bool x64_emit_call_ins(X64CodegenCtx *ctx, XirIns *ins, X64Reg rd);

#endif // XIR_CODEGEN_X64_INTERNAL_H
