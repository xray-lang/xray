/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_internal.h - Internal shared declarations for Xm codegen
 */

#ifndef XM_CODEGEN_INTERNAL_H
#define XM_CODEGEN_INTERNAL_H

#include "xm_codegen.h"
#include "xm_arm64.h"
#include "xm_jit.h"
#include "xm_regalloc.h"
#include "../runtime/value/xslot_type.h"
#include "xm_target.h"
#include "xm_jit_runtime.h"
#include "xm_offsets.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../base/xdefs.h"

/* ========== Constants ========== */

#define MAX_PHYS_REGS 22
#define MAX_FP_REGS 16
#define MAX_VREGS 4096
#define SCRATCH_REG A64_X16
#define SCRATCH_REG2 A64_X17
#define CORO_REG A64_X19
#define SAFEPT_PAGE_REG A64_X20  // guard page pointer (reserved, loaded in prologue)
#define JIT_CTX_REG A64_X28

// Frame-embedded GC stack map metadata (after callee-saved area)
#define FRAME_SMAP_PTR_OFFSET 160  // XrStackMapTable* for this JIT function
#define FRAME_SMAP_ID_OFFSET 168   // safepoint_id (uint32_t) at current point

#define JIT_FRAME_BASE 176
#define SPILL_BASE 176

/* ========== Branch Patching ========== */

typedef enum {
    PATCH_B,
    PATCH_CBNZ,
    PATCH_CBZ,
    PATCH_SAFEPOINT,
    PATCH_BARRIER_FWD,
    PATCH_BARRIER_BACK,
    PATCH_DEOPT,
    PATCH_DEOPT_NE,
    PATCH_DEOPT_EQ,
    PATCH_DEOPT_CBZ,
    PATCH_DEOPT_CBNZ,
    PATCH_DEOPT_CS,
    PATCH_CALL_C,
    PATCH_CALL_SELF,
    PATCH_CALL_SELF_FAST,
    PATCH_B_COND,
} PatchType;

typedef struct {
    uint32_t emit_idx;
    uint32_t target_blk;
    PatchType type;
    A64Reg reg;
} BranchPatch;

#define INIT_PATCHES 256
#define INIT_CS_PATCHES 256

/* ========== Codegen Context ========== */

typedef struct {
    uint32_t block_id;
    uint32_t block_offset;
} OsrSnapshot;

typedef struct {
    uint32_t idx;
    uint8_t pair;
} CsPatch;

typedef struct {
    XmFunc *func;
    XmCodeAlloc *alloc;
    A64Buf buf;

    uint32_t *block_offsets;
    uint32_t nblock_offsets;

    BranchPatch *patches;
    uint32_t npatch;
    uint32_t patches_cap;
    uint32_t safepoint_stub;
    uint32_t barrier_fwd_stub;
    uint32_t barrier_back_stub;
    uint32_t deopt_stub;
    uint32_t call_c_stub;
    bool has_safepoints;
    bool has_barriers;
    bool has_deopt;
    bool has_call_c;

    OsrSnapshot osr_snaps[XM_MAX_OSR_ENTRIES];
    uint32_t nosr_snap;

    uint32_t frame_patch_sub[8];
    uint32_t frame_patch_add[32];
    uint32_t nsub_patches;
    uint32_t nadd_patches;

    uint32_t fast_entry_offset;

    CsPatch *cs_patches;
    uint32_t ncs_patches;
    uint32_t cs_patches_cap;

    XraResult *xra;
    uint32_t cur_blk_id;
    int32_t cur_ra_pos;        // current RA position for segment lookup
    uint32_t cur_ins_idx;      // current instruction index in block
    uint32_t gap_move_cursor;  // index into xra->gap_moves for current block
    int8_t *vreg_override;  // [nvreg] per-vreg register override from gap moves, -128=no override

    XmRef fused_cmp_ref;
    A64Cond fused_false_cc;
    bool fused_is_float;

    // Graceful error: set when regalloc has no assignment for a vreg
    bool had_error;

    // GC stack map: collect safepoint bitmaps during codegen
    XrStackMapEntry smap_entries[XM_MAX_STACK_MAP_ENTRIES];
    uint32_t nsmap;

    // Suspend/resume tracking
    uint32_t suspend_cont_offsets[16];    // code offset of continuation point per suspend_id
    uint32_t suspend_smap_ids[16];        // stack map id at each suspend point
    uint8_t suspend_result_regs[16];      // physical register for await result per suspend_id
    int16_t suspend_result_bc_slots[16];  // bc_slot of await result vreg per suspend_id
    int32_t suspend_result_tag_offs[16];  // vreg_runtime_tags offset per suspend_id (-1=none)
    uint32_t nsuspend;                    // number of suspend points emitted
    uint32_t resume_entry_offset;         // code offset of resume entry (0 = none)
} CodegenCtx;

// Register allocation tables (defined in xm_codegen.c)
extern const A64Reg alloc_regs[MAX_PHYS_REGS];
extern const A64Reg alloc_fp_regs[MAX_FP_REGS];

/* ========== Shared helpers (defined in xm_codegen.c) ========== */

XR_FUNC A64Reg xra_get(CodegenCtx *ctx, XmRef ref);
XR_FUNC A64Reg xra_arg(CodegenCtx *ctx, XmRef ref, A64Reg scratch_reg);
XR_FUNC A64Reg xra_operand(CodegenCtx *ctx, XmRef ref, A64Reg scratch_reg);
XR_FUNC void xra_maybe_spill(CodegenCtx *ctx, XmRef dst_ref);
XR_FUNC int xra_live_gp(CodegenCtx *ctx, A64Reg *out, A64Reg exclude_reg);
XR_FUNC int xra_live_fp(CodegenCtx *ctx, A64Reg *out);
XR_FUNC int xra_live_ptr(CodegenCtx *ctx, A64Reg *out);
XR_FUNC void emit_load_const(CodegenCtx *ctx, A64Reg rd, XmRef ref);
XR_FUNC uint32_t record_safepoint(CodegenCtx *ctx);
XR_FUNC void emit_ptr_spill_writeback(CodegenCtx *ctx);
XR_FUNC void emit_jit_frame_push(CodegenCtx *ctx);
XR_FUNC void emit_jit_frame_pop(CodegenCtx *ctx);
XR_FUNC void add_patch(CodegenCtx *ctx, PatchType type, uint32_t target_blk, A64Reg reg);
XR_FUNC void add_cs_patch(CodegenCtx *ctx, uint8_t pair);

// Epilogue emission (defined in xm_codegen.c, used by XM_SUSPEND in xm_codegen_mem.c)
XR_FUNC void emit_epilogue(CodegenCtx *ctx);

/* ========== Sub-emit functions (defined in split files) ========== */

XR_FUNC void a64_emit_xm_ins(CodegenCtx *ctx, XmIns *ins);
XR_FUNC bool xm_emit_call_ops(CodegenCtx *ctx, XmIns *ins, A64Reg rd);
XR_FUNC bool xm_emit_mem_ops(CodegenCtx *ctx, XmIns *ins, A64Reg rd);

/* Stubs, OSR, patching, deopt table, resume (xm_codegen_stub.c) */
XR_FUNC void a64_emit_barrier_stubs(CodegenCtx *ctx);
XR_FUNC void a64_emit_deopt_stub(CodegenCtx *ctx);
XR_FUNC void a64_emit_call_c_stub(CodegenCtx *ctx);
XR_FUNC void a64_emit_osr_stubs(CodegenCtx *ctx, XmCodegenResult *result);
XR_FUNC void a64_patch_branches(CodegenCtx *ctx);
XR_FUNC void a64_build_runtime_deopt_table(CodegenCtx *ctx, XmCodegenResult *result);
XR_FUNC void a64_emit_resume_entry(CodegenCtx *ctx, XmCodegenResult *result);

#endif  // XM_CODEGEN_INTERNAL_H
