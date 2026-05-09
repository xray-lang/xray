/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_x64_internal.h - Internal types for x86-64 codegen
 *
 * KEY CONCEPT:
 *   Shared state between xm_codegen_x64.c and its sub-files.
 *   Mirrors the role of xm_codegen_internal.h for ARM64.
 */

#ifndef XM_CODEGEN_X64_INTERNAL_H
#define XM_CODEGEN_X64_INTERNAL_H

#include <setjmp.h>
#include "xm_codegen.h"
#include "xm_codegen_internal.h"
#include "xm_x64.h"
#include "xm_jit.h"
#include "xm_regalloc.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../base/xlog.h"

/* ========== Constants ========== */

#define X64_MAX_PHYS_REGS 11  // allocatable GP registers (see xm_target_x64.c)
#define X64_MAX_FP_REGS 15
#define X64_MAX_VREGS 4096
#define X64_SCRATCH_REG X64_R11
#define X64_SCRATCH_XMM 15  // xmm15 as FP scratch
#define X64_CORO_REG X64_R15
#define X64_JIT_CTX_REG X64_R14  // jit_ctx pointer (XrJitScratch*)

/* ========== Platform ABI Abstraction ========== */

#ifdef _WIN32
/*
 * Win64 (Microsoft x64) calling convention:
 *   Integer args:  RCX, RDX, R8, R9   (only 4 register args)
 *   FP args:       XMM0-XMM3
 *   Return:        RAX  (struct >8B via hidden first-arg pointer)
 *   Caller-saved:  RAX, RCX, RDX, R8, R9, R10, R11, XMM0-XMM5
 *   Callee-saved:  RBX, RDI, RSI, RBP, R12-R15, XMM6-XMM15
 *   Shadow space:  32 bytes above return address (reserved by caller)
 *   No red zone.
 */
#define X64_ABI_ARG1 X64_RCX
#define X64_ABI_ARG2 X64_RDX
#define X64_ABI_ARG3 X64_R8
#define X64_ABI_ARG4 X64_R9
#define X64_ABI_SHADOW_BYTES 32
#define X64_ABI_STRUCT_RET_SPACE 16                     /* XrJitResult return buffer for C calls */
#define X64_NGPR_CALLER_SAVE 6                          /* rax,rcx,rdx,r8,r9,r10 */
#define X64_NGPR_CALLEE_SAVE_ALLOC 5                    /* rbx,rdi,rsi,r12,r13 */
#define X64_NPUSH_CALLEE_SAVE 7                         /* rbx,rdi,rsi,r12,r13,r14,r15 */
#define X64_NFPR_CALLER_SAVE 6                          /* xmm0-xmm5 */
#define X64_CALLEE_XMM_COUNT 9                          /* xmm6-xmm14 (xmm15=scratch) */
#define X64_CALLEE_XMM_BYTES (X64_CALLEE_XMM_COUNT * 8) /* MOVSD, 8B each */
#else
/*
 * System V AMD64 ABI:
 *   Integer args:  RDI, RSI, RDX, RCX, R8, R9
 *   FP args:       XMM0-XMM7
 *   Return:        RAX + RDX  (struct <=16B in two registers)
 *   Caller-saved:  RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, XMM0-XMM15
 *   Callee-saved:  RBX, RBP, R12-R15
 *   Red zone:      128 bytes below RSP (leaf functions only)
 */
#define X64_ABI_ARG1 X64_RDI
#define X64_ABI_ARG2 X64_RSI
#define X64_ABI_ARG3 X64_RDX
#define X64_ABI_ARG4 X64_RCX
#define X64_ABI_SHADOW_BYTES 0
#define X64_ABI_STRUCT_RET_SPACE 0
#define X64_NGPR_CALLER_SAVE 8       /* rax,rcx,rdx,rsi,rdi,r8,r9,r10 */
#define X64_NGPR_CALLEE_SAVE_ALLOC 3 /* rbx,r12,r13 */
#define X64_NPUSH_CALLEE_SAVE 5      /* rbx,r12,r13,r14,r15 */
#define X64_NFPR_CALLER_SAVE 15      /* all xmm caller-saved */
#define X64_CALLEE_XMM_COUNT 0
#define X64_CALLEE_XMM_BYTES 0
#endif

/*
 * Frame layout (x86-64, both ABIs):
 *
 *   [higher addresses]
 *     return address      (pushed by CALL)
 *     saved rbp           (PUSH rbp)             ← rbp points here
 *     --- SUB RSP, frame_size ---
 *     [stack_map_ptr]     -8
 *     [safepoint_id]      -16
 *     [XMM callee-save area: Win64 only, 9×8=72 bytes at -24..-96]
 *     --- spill slots ---
 *     --- PUSH callee-saved GP regs ---           ← RSP after prologue
 *
 * SysV:  5 GP pushes (rbx,r12,r13,r14,r15)  = 40B, frame_base=16
 * Win64: 7 GP pushes (rbx,rdi,rsi,r12,r13,r14,r15) = 56B, frame_base=16+72=88
 *
 * SPILL_BASE = frame metadata + XMM save area
 */
/* Frame-embedded GC stack map metadata (negative offsets from RBP).
 * These mirror ARM64's FRAME_SMAP_PTR_OFFSET / FRAME_SMAP_ID_OFFSET.
 * Usage: x64_mov_rm(buf, reg, RBP, -(int32_t)X64_FRAME_SMAP_PTR_OFFSET) */
#define X64_FRAME_SMAP_PTR_OFFSET  8   /* [rbp - 8]:  XrStackMapTable* for this JIT function */
#define X64_FRAME_SMAP_ID_OFFSET  16   /* [rbp - 16]: safepoint_id (uint32_t) at current point */

#ifdef _WIN32
#define X64_SPILL_BASE (16 + X64_CALLEE_XMM_BYTES) /* 16 + 72 = 88 */
#define X64_JIT_FRAME_BASE (16 + X64_CALLEE_XMM_BYTES)
#define X64_XMM_SAVE_OFFSET 24 /* first XMM save at [rbp - 24] */
#else
#define X64_SPILL_BASE 64
#define X64_JIT_FRAME_BASE 64
#endif

/* ========== Branch Patch ========== */

/* Extra-arg scratch: reuse the call_args[15] slot to pass extra_arg to
 * call_c_stub. Safe because call_args are written before the call and
 * the extra_arg is consumed before any call_args are read by the callee. */
#define X64_EXTRA_ARG_OFFSET XM_JIT_TAG_SCRATCH_OFFSET

typedef enum {
    X64_PATCH_JMP,             // unconditional JMP rel32
    X64_PATCH_JCC,             // conditional Jcc rel32
    X64_PATCH_DEOPT_JCC,       // deopt: conditional Jcc to deopt stub
    X64_PATCH_DEOPT_JMP,       // deopt: unconditional JMP rel32 to deopt stub
    X64_PATCH_CALL_C,          // CALL rel32 to shared call_c_stub
    X64_PATCH_CALL_SELF,       // CALL rel32 to function entry (offset 0)
    X64_PATCH_CALL_SELF_FAST,  // CALL rel32 to fast_entry_offset
    X64_PATCH_BARRIER_FWD,     // CALL rel32 to barrier_fwd_stub
    X64_PATCH_BARRIER_BACK,    // CALL rel32 to barrier_back_stub
} X64PatchType;

typedef struct {
    uint32_t emit_pos;    // byte offset of the rel32 field in code buffer
    uint32_t target_blk;  // target block id
    X64PatchType type;
    X64Cond cc;  // condition code (for JCC patches)
} X64BranchPatch;

#define X64_INIT_PATCHES 256

/* ========== Codegen Context ========== */

typedef struct {
    XmFunc *func;
    XmCodeAlloc *alloc;
    X64Buf buf;

    uint32_t *block_offsets;  // byte offset of each block's start
    uint32_t nblock_offsets;

    X64BranchPatch *patches;  // deferred branch patches
    uint32_t npatch;
    uint32_t patches_cap;

    XraResult *xra;         // register allocation result
    int8_t *vreg_override;  // gap-move overrides (-128 = no override)

    uint32_t cur_blk_id;
    int32_t cur_ra_pos;
    uint32_t cur_ins_idx;
    uint32_t gap_move_cursor;

    uint32_t fast_entry_offset;  // byte offset of fast-path entry

    /* Frame size patch locations (byte offsets where sub rsp/add rsp imm32 lives).
     * Capacity 16 covers normal + fast prologue + up to XM_MAX_OSR_ENTRIES (8)
     * OSR stubs, each of which emits its own SUB RSP, imm32. */
    uint32_t frame_patch_sub[16];
    uint32_t frame_patch_add[8];
    uint32_t nsub_patches;
    uint32_t nadd_patches;

    /* Loop-header snapshots for later OSR stub emission. Filled by
     * x64_emit_block when it sees a loop_header block; consumed by
     * x64_emit_osr_stubs to materialize a stub per snapshot. */
    OsrSnapshot osr_snaps[XM_MAX_OSR_ENTRIES];
    uint32_t nosr_snap;

    uint32_t call_c_stub;        // byte offset of call_c_stub in code buffer
    uint32_t deopt_stub;         // byte offset of deopt stub
    uint32_t barrier_fwd_stub;   // byte offset of forward barrier stub
    uint32_t barrier_back_stub;  // byte offset of back barrier stub

    /* GC stack map: collect safepoint bitmaps during codegen */
    XrStackMapEntry smap_entries[XM_MAX_STACK_MAP_ENTRIES];
    uint32_t nsmap;

    bool had_error;
    const char *error_reason;
    jmp_buf bail_jmp;

    bool has_deopt;
    bool has_call_c;
    bool has_barriers;

    /* Suspend/resume tracking (coroutine support) */
    uint32_t suspend_cont_offsets[16];    // byte offset of continuation per suspend_id
    uint32_t suspend_smap_ids[16];        // smap id at each suspend point
    uint8_t suspend_result_regs[16];      // physical register for result per suspend_id
    int16_t suspend_result_bc_slots[16];  // bc_slot of result vreg per suspend_id
    int32_t suspend_result_tag_offs[16];  // vreg_runtime_tags offset per suspend_id (-1=none)
    uint32_t nsuspend;                    // number of suspend points emitted
    uint32_t resume_entry_offset;         // byte offset of resume entry (0 = none)
} X64CodegenCtx;

/* ========== Register Mapping ========== */

/* Map physical register index (0..12) to x86-64 hardware register */
extern const X64Reg x64_alloc_regs[X64_MAX_PHYS_REGS];
extern const X64Reg x64_alloc_fp_regs[X64_MAX_FP_REGS];

/* Get the x86-64 GP hardware register assigned to a vreg reference */
XR_FUNC X64Reg x64_get_reg(X64CodegenCtx *ctx, XmRef ref);

/* Get GP register for a source operand, loading from spill slot if needed */
XR_FUNC X64Reg x64_get_operand(X64CodegenCtx *ctx, XmRef ref, X64Reg scratch);

/* Get the FP (xmm) register assigned to a vreg reference */
XR_FUNC X64Xmm x64_get_fp_reg(X64CodegenCtx *ctx, XmRef ref);

/* Get FP register for a source operand, loading constant or spill if needed */
XR_FUNC X64Xmm x64_get_fp_operand(X64CodegenCtx *ctx, XmRef ref, X64Xmm scratch);

/* Load a 64-bit constant into a register using optimal encoding */
XR_FUNC void x64_load_imm64(X64Buf *buf, X64Reg dst, uint64_t val);

/* Store to spill slot if vreg has one assigned (GP or FP) */
XR_FUNC void x64_maybe_spill(X64CodegenCtx *ctx, XmRef dst_ref);

/* Add a deferred branch patch */
XR_FUNC void x64_add_patch(X64CodegenCtx *ctx, X64PatchType type, uint32_t target_blk, X64Cond cc);

/* Derive XR_TAG_* from const rep for call_arg_tags[] */
static inline uint8_t const_rep_to_value_tag(uint8_t rep) {
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

/* ========== Shared helpers (defined in xm_codegen_x64.c) ========== */

/* Write call arguments from pool to jit_ctx->call_args[] */
XR_FUNC void x64_emit_call_args_from_pool(X64CodegenCtx *ctx, XmIns *ins);

/* Write live PTR-typed registers back to their spill slots for GC visibility */
XR_FUNC void x64_emit_ptr_spill_writeback(X64CodegenCtx *ctx);

/* Record a safepoint GC stack map bitmap, return the smap_id */
XR_FUNC uint32_t x64_record_safepoint(X64CodegenCtx *ctx);

/* Collect live caller-saved GP registers (exclude one), return count */
XR_FUNC int x64_live_gp(X64CodegenCtx *ctx, X64Reg *out, X64Reg exclude);

/* Collect live FP registers, return count */
XR_FUNC int x64_live_fp(X64CodegenCtx *ctx, X64Xmm *out);

/* Emit deopt_id store (for deopt stub identification) */
XR_FUNC void x64_emit_deopt_id(X64CodegenCtx *ctx, XmIns *ins);

/* Emit conditional deopt jump (Jcc to deopt stub) */
XR_FUNC void x64_emit_deopt_jcc(X64CodegenCtx *ctx, X64Cond cc);

/* Emit epilogue sequence (restore callee-saved regs, leave, ret) */
XR_FUNC void x64_emit_epilogue(X64CodegenCtx *ctx);

/* Check whether a vreg holds a float-rep value */
static inline bool x64_is_fp_vreg(X64CodegenCtx *ctx, XmRef ref) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= ctx->func->nvreg)
        return false;
    return ctx->func->vregs[idx].rep == XR_REP_F64;
}

/* ========== Sub-emit functions (defined in split files) ========== */

/* Per-instruction emission (xm_codegen_x64_ins.c) */
XR_FUNC void x64_emit_xm_ins(X64CodegenCtx *ctx, XmIns *ins);

/* Inline alloc fast path (xm_codegen_x64_mem.c) */
XR_FUNC void x64_emit_alloc_ins(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd,
                                 uint8_t gc_type, uint16_t gc_extra,
                                 uint32_t alloc_size);

/* Edge copies + branch patching (xm_codegen_x64_patch.c) */
XR_FUNC void x64_emit_edge_copies(X64CodegenCtx *ctx, XmBlock *target,
                                    XmBlock *from);
XR_FUNC void x64_patch_branches(X64CodegenCtx *ctx);

/* Call ops: CALL_C, CALL_C_LEAF, CALL_SELF_DIRECT, CALL_KNOWN, etc. */
XR_FUNC bool x64_emit_call_ins(X64CodegenCtx *ctx, XmIns *ins, X64Reg rd);

/* Stubs: call_c, barriers, deopt (xm_codegen_x64_stub.c) */
XR_FUNC void x64_emit_call_c_stub(X64CodegenCtx *ctx);
XR_FUNC void x64_emit_barrier_stubs(X64CodegenCtx *ctx);
XR_FUNC void x64_emit_deopt_stub(X64CodegenCtx *ctx);

/* OSR + resume entry (xm_codegen_x64_osr.c) */
XR_FUNC void x64_emit_osr_stubs(X64CodegenCtx *ctx, XmCodegenResult *result);
XR_FUNC void x64_emit_resume_entry(X64CodegenCtx *ctx, XmCodegenResult *result);

/* Bail out of codegen on invariant violation instead of abort().
 * Sets had_error and longjmps to the entry point of xm_codegen_x64
 * so the caller can report failure gracefully. */
#define CODEGEN_CHECK(ctx, cond, msg)                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (ctx)->had_error = true;                                                               \
            (ctx)->error_reason = (msg);                                                           \
            xr_log_warning("x64-cg", "bail: %s (%s:%d)", (msg), __FILE__, __LINE__);               \
            longjmp((ctx)->bail_jmp, 1);                                                           \
        }                                                                                          \
    } while (0)

#endif  // XM_CODEGEN_X64_INTERNAL_H
