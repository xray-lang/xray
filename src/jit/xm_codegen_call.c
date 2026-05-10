/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_call.c - Xm codegen for call/invoke instructions
 */

#ifdef __aarch64__

#include "xm_codegen_internal.h"
#include "../base/xchecks.h"

// Derive runtime XR_TAG_* from const rep for call_arg_tags[].
static inline uint8_t const_rep_to_value_tag(uint8_t rep) {
    switch (rep) {
        case XR_REP_I64:
            return 3;  // XR_TAG_I64
        case XR_REP_F64:
            return 4;  // XR_TAG_F64
        case XR_REP_PTR:
            return 5;  // XR_TAG_PTR
        default:
            return 0xFF;  // XR_RTAG_UNKNOWN
    }
}

// Write call arguments from the pool to jit_ctx->call_args[] and
// compile-time type tags to jit_ctx->call_arg_tags[].
// Tags are derived from vreg ctype (precise) or const rep, packed into
// 1-2 uint64_t constants for efficient storage (no per-arg STRB needed).
static void emit_call_args_from_pool(CodegenCtx *ctx, XmIns *ins) {
    if (!xm_ref_is_vreg(ins->dst))
        return;
    uint32_t vi = XM_REF_INDEX(ins->dst);
    if (vi >= ctx->func->nvreg)
        return;
    XmVReg *vreg = &ctx->func->vregs[vi];
    if (vreg->call_nargs == 0)
        return;
    XmRef *pool = ctx->func->call_arg_pool;
    uint32_t start = vreg->call_arg_start;

    uint64_t tag_pack[2] = {0, 0};

    for (uint16_t i = 0; i < vreg->call_nargs; i++) {
        XmRef arg = pool[start + i];
        int32_t off = (int32_t) (XM_JIT_CALL_ARGS_OFFSET + i * 8);
        uint8_t tag = XR_RTAG_UNKNOWN;
        if (xm_ref_is_none(arg))
            goto store_tag;
        if (xm_ref_is_const(arg)) {
            uint32_t ci = XM_REF_INDEX(arg);
            uint64_t val = (uint64_t) ctx->func->consts[ci].val.raw;
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, val);
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, off));
            tag = const_rep_to_value_tag(ctx->func->consts[ci].rep);
        } else {
            A64Reg reg = xra_arg(ctx, arg, SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_str(reg, JIT_CTX_REG, off));
            XmType ct = xm_ref_ctype(ctx->func, arg);
            tag = vtag_to_value_tag(type_kind_to_vtag(ct.kind));
        }
    store_tag:
        if (i < 8)
            tag_pack[0] |= ((uint64_t) tag << (i * 8));
        else
            tag_pack[1] |= ((uint64_t) tag << ((i - 8) * 8));
    }

    // Store packed tags as compile-time constants (1-2 immediate loads)
    a64_load_imm64(&ctx->buf, SCRATCH_REG2, tag_pack[0]);
    a64_buf_emit(&ctx->buf,
                 a64_str(SCRATCH_REG2, JIT_CTX_REG, (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET));
    if (vreg->call_nargs > 8) {
        a64_load_imm64(&ctx->buf, SCRATCH_REG2, tag_pack[1]);
        a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG,
                                        (int32_t) (XM_JIT_CALL_ARG_TAGS_OFFSET + 8)));
    }

    // Dynamic patch: for args with UNKNOWN compile-time tag, load the
    // precise runtime tag from vreg_runtime_tags[vreg_idx] and overwrite
    // the 0xFF byte in call_arg_tags[i]. Indexed by vreg, no bc_slot.
    for (uint16_t i = 0; i < vreg->call_nargs; i++) {
        uint8_t ct = (i < 8) ? (uint8_t) ((tag_pack[0] >> (i * 8)) & 0xFF)
                             : (uint8_t) ((tag_pack[1] >> ((i - 8) * 8)) & 0xFF);
        if (ct != XR_RTAG_UNKNOWN)
            continue;
        XmRef arg = pool[start + i];
        if (!xm_ref_is_vreg(arg))
            continue;
        uint32_t ai = XM_REF_INDEX(arg);
        if (ai >= ctx->func->nvreg || ai >= XR_JIT_MAX_VREG_TAGS)
            continue;
        int32_t src_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) ai;
        int32_t dst_off = (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET + (int32_t) i;
        a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG, src_off));
        a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, dst_off));
    }
}

bool xm_emit_call_ops(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    XR_DCHECK(ctx != NULL, "emit_call_ops: NULL ctx");
    XR_DCHECK(ins != NULL, "emit_call_ops: NULL ins");
    switch (ins->op) {
        // CALL_C: call C runtime function via shared stub
        // args[0] = const_ptr(function_address)
        // args[1] = first extra argument (optional, passed as x1)
        // Stub convention: x16=func_ptr, x17=extra_arg, x0=coro
        case XM_CALL_C: {
            emit_call_args_from_pool(ctx, ins);
            // Write live PTR registers back to spill slots before the call.
            // record_safepoint sets spill_bitmap for any PTR vreg that has a spill
            // slot (even when it is currently in a register), so GC will read those
            // slots.  Without this writeback the slots hold stale/garbage data and
            // GC crashes when it tries to markobject on the garbage pointer.
            emit_ptr_spill_writeback(ctx);
            // Record stack map bitmap + emit store safepoint_id
            // GC uses this bitmap + call_c_stub's saved x1-x15 to find PTR roots
            uint32_t smap_id_cc = record_safepoint(ctx);
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id_cc, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            // Load function pointer to x16
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                uint64_t fn_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
                a64_load_imm64(&ctx->buf, SCRATCH_REG, fn_ptr);
            } else {
                A64Reg fn_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, fn_reg));
            }
            // Load extra arg to x17 (if present)
            if (!xm_ref_is_none(ins->args[1])) {
                if (xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    uint64_t arg_val = (uint64_t) ctx->func->consts[ci].val.raw;
                    a64_load_imm64(&ctx->buf, SCRATCH_REG2, arg_val);
                } else {
                    A64Reg arg_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                    a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, arg_reg));
                }
            }
            // Clear deopt_id before CALL_C so helpers can request deopt
            a64_buf_emit(&ctx->buf, a64_str_w(A64_XZR, JIT_CTX_REG, XM_JIT_DEOPT_ID_OFFSET));
            // BL to call_c_stub (patched later)
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;
            // Check if C helper requested deopt (e.g. yieldable cfunc)
            a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_DEOPT_ID_OFFSET));
            add_patch(ctx, PATCH_DEOPT_CBNZ, 0, SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_deopt = true;
            // stub returns x0=payload; tag is in jit_ctx->call_result_tag
            // (call_c_stub stores C helper's x1 there to avoid clobbering
            // alloc_regs[0]=x1 which may hold a live vreg across the call)
            // Move payload to dst register
            if (rd != A64_XZR) {
                bool dst_fp = false;
                if (xm_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XM_REF_INDEX(ins->dst);
                    if (vi < ctx->func->nvreg)
                        dst_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
                }
                if (dst_fp) {
                    a64_buf_emit(&ctx->buf, a64_fmov_from_gpr(rd, A64_X0));
                } else {
                    a64_buf_emit(&ctx->buf, a64_mov(rd, A64_X0));
                }
            }
            // Store tag (from jit_ctx->call_result_tag) to vreg_runtime_tags[vi]
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS) {
                    int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG,
                                                     XM_JIT_CALL_RESULT_TAG_OFFSET));
                    a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag_off));
                }
            }
            break;
        }

        // CALL_C_LEAF: lightweight C call with precise register save/restore.
        // Same args convention as CALL_C: args[0]=func_ptr, args[1]=extra_arg.
        // Callee is a leaf function (no GC, no JIT re-entry).
        // Only saves actually-live caller-saved GP/FP registers.
        case XM_CALL_C_LEAF: {
            emit_call_args_from_pool(ctx, ins);
            // Load function pointer to x16
            uint64_t fn_ptr_val = 0;
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                fn_ptr_val = (uint64_t) ctx->func->consts[ci].val.raw;
            }
            // Load extra arg value (constant or register)
            uint64_t extra_arg_val = 0;
            bool extra_is_const = false;
            A64Reg extra_reg = A64_XZR;
            if (!xm_ref_is_none(ins->args[1])) {
                if (xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    extra_arg_val = (uint64_t) ctx->func->consts[ci].val.raw;
                    extra_is_const = true;
                } else {
                    extra_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                }
            }

            // Collect live caller-saved regs from XraResult
            A64Reg live_gp[15];
            int ngp = xra_live_gp(ctx, live_gp, rd);
            A64Reg live_fp[8];
            int nfp = xra_live_fp(ctx, live_fp);

            // Save live regs to stack (16-byte aligned)
            int32_t save_frame = ((ngp + nfp) * 8 + 15) & ~15;
            if (save_frame > 0) {
                a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, save_frame));
                int off = 0, si = 0;
                while (si + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_stp(live_gp[si], live_gp[si + 1], A64_SP, off));
                    off += 16;
                    si += 2;
                }
                if (si < ngp) {
                    a64_buf_emit(&ctx->buf, a64_str(live_gp[si], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_str_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
            }

            // Setup call: x0=coro, x1=extra_arg
            a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
            if (!xm_ref_is_none(ins->args[1])) {
                if (extra_is_const) {
                    a64_load_imm64(&ctx->buf, A64_X1, extra_arg_val);
                } else {
                    a64_buf_emit(&ctx->buf, a64_mov(A64_X1, extra_reg));
                }
            }

            // Load function pointer and BLR directly (no shared stub)
            a64_load_imm64(&ctx->buf, SCRATCH_REG, fn_ptr_val);
            a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));

            // Save return value to x16
            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, A64_X0));

            // Restore live regs
            if (save_frame > 0) {
                int off = 0, si = 0;
                while (si + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldp(live_gp[si], live_gp[si + 1], A64_SP, off));
                    off += 16;
                    si += 2;
                }
                if (si < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldr(live_gp[si], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_ldr_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
                a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, save_frame));
            }

            // Move result from x16 to dst
            if (rd != A64_XZR) {
                a64_buf_emit(&ctx->buf, a64_mov(rd, SCRATCH_REG));
            }
            break;
        }

        // CALL_SELF_DIRECT: direct recursive self-call (BL to own entry)
        // Two modes:
        //   Register passing (args[0] != NONE): args in registers, BL fast_entry
        //   Memory passing   (args[0] == NONE): args via STORE_CORO, BL entry
        case XM_CALL_SELF_DIRECT: {
            emit_call_args_from_pool(ctx, ins);
            // Write live PTR regs to spill slots for GC visibility in caller frame
            emit_ptr_spill_writeback(ctx);

            // Record stack map bitmap + emit store safepoint_id
            uint32_t smap_id_self = record_safepoint(ctx);
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id_self, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            bool reg_passing = !xm_ref_is_none(ins->args[0]);

            // Resolve arg registers before save (may trigger spill)
            A64Reg arg_regs[2] = {A64_XZR, A64_XZR};
            int nargs_reg = 0;
            if (reg_passing) {
                A64Reg scratches[2] = {SCRATCH_REG, SCRATCH_REG2};
                for (int a = 0; a < 2; a++) {
                    if (xm_ref_is_none(ins->args[a]))
                        break;
                    arg_regs[a] = xra_arg(ctx, ins->args[a], scratches[a]);
                    nargs_reg++;
                }
            }

            // Collect live caller-saved regs from XraResult
            A64Reg live_gp[15];
            int ngp = xra_live_gp(ctx, live_gp, rd);
            A64Reg live_fp[8];
            int nfp = xra_live_fp(ctx, live_fp);

            int32_t save_frame = (((ngp + nfp) * 8 + 15) & ~15);

            if (save_frame > 0) {
                a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, save_frame));
                int off = 0, i = 0;
                while (i + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_stp(live_gp[i], live_gp[i + 1], A64_SP, off));
                    off += 16;
                    i += 2;
                }
                if (i < ngp) {
                    a64_buf_emit(&ctx->buf, a64_str(live_gp[i], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_str_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
            }

            if (reg_passing) {
                // Move args to fixed ABI registers (alloc_regs[0..N]).
                if (nargs_reg == 1) {
                    A64Reg p0 = alloc_regs[0];
                    if (arg_regs[0] != p0)
                        a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[0]));
                } else if (nargs_reg == 2) {
                    A64Reg p0 = alloc_regs[0];
                    A64Reg p1 = alloc_regs[1];
                    if (arg_regs[0] == p1 && arg_regs[1] == p0) {
                        a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, arg_regs[0]));
                        a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[1]));
                        a64_buf_emit(&ctx->buf, a64_mov(p1, SCRATCH_REG));
                    } else if (arg_regs[1] == p0) {
                        if (arg_regs[1] != p1)
                            a64_buf_emit(&ctx->buf, a64_mov(p1, arg_regs[1]));
                        if (arg_regs[0] != p0)
                            a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[0]));
                    } else {
                        if (arg_regs[0] != p0)
                            a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[0]));
                        if (arg_regs[1] != p1)
                            a64_buf_emit(&ctx->buf, a64_mov(p1, arg_regs[1]));
                    }
                }
                // Load extra args (arg2..N-1) from call_args[2..N-1] to alloc_regs[2..N-1].
                // These were pre-stored by STORE_CORO in the builder.
                int extra = XM_FLAG_EXTRA_ARGS_GET(ins->flags);
                for (int ei = 0; ei < extra; ei++) {
                    A64Reg dst = alloc_regs[2 + ei];
                    int32_t off = (int32_t) (XM_JIT_CALL_ARGS_OFFSET + (2 + ei) * 8);
                    a64_buf_emit(&ctx->buf, a64_ldr(dst, JIT_CTX_REG, off));
                }
                a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
                add_patch(ctx, PATCH_CALL_SELF_FAST, 0, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());
            } else {
                a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
                a64_buf_emit(&ctx->buf, a64_add_imm(A64_X1, JIT_CTX_REG, XM_JIT_CALL_ARGS_OFFSET));
                add_patch(ctx, PATCH_CALL_SELF, 0, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());
            }

            // Restore caller's stack map in jit_ctx (no frame push/pop needed:
            // self-recursive calls have continuous FP chain, GC walks it directly)
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, A64_FP, FRAME_SMAP_PTR_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));

            // Save return value (x0) to x16
            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, A64_X0));

            // Store tag (x1 = callee's tag from RET epilogue) to vreg_runtime_tags
            // before deopt check clobbers x17 (SCRATCH_REG2) with DEOPT_MARKER.
            int32_t self_vreg_off = -1;
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS)
                    self_vreg_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
            }
            if (self_vreg_off >= 0) {
                a64_buf_emit(&ctx->buf, a64_strb(A64_X1, JIT_CTX_REG, self_vreg_off));
            }

            // Deopt propagation check
            {
                a64_load_imm64(&ctx->buf, SCRATCH_REG2, (uint64_t) XM_DEOPT_MARKER);
                a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG, SCRATCH_REG2));
                uint32_t bne_idx = ctx->buf.count;
                a64_buf_emit(&ctx->buf, a64_nop());

                if (save_frame > 0)
                    a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, save_frame));
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, 0xFFFF, 0));
                add_patch(ctx, PATCH_DEOPT, 0, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());
                ctx->has_deopt = true;

                int32_t skip_off = (int32_t) ctx->buf.count - (int32_t) bne_idx;
                ctx->buf.code[bne_idx] = a64_b_cond(A64_CC_NE, skip_off);
            }

            if (save_frame > 0) {
                int off = 0, i = 0;
                while (i + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldp(live_gp[i], live_gp[i + 1], A64_SP, off));
                    off += 16;
                    i += 2;
                }
                if (i < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldr(live_gp[i], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_ldr_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
                a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, save_frame));
            }

            if (rd != A64_XZR) {
                a64_buf_emit(&ctx->buf, a64_mov(rd, SCRATCH_REG));
            }
            // Reload tag from vreg_runtime_tags to x1: live-reg restore above
            // clobbered x1. x1 must hold callee's tag for the CALLEE_SETS chain
            // (caller's RET epilogue skips overwriting x1 when CALLEE_SETS).
            if (self_vreg_off >= 0) {
                a64_buf_emit(&ctx->buf, a64_ldrb(A64_X1, JIT_CTX_REG, self_vreg_off));
            }
            break;
        }

        // CALL_DIRECT: cross-function JIT→JIT call with inline fast path
        // args[0] = nargs (vreg), args[1] = xr_jit_call_func ptr (fallback)
        // Fast path: load closure→proto→jit_entry from jit_call_args[0],
        //   if jit_entry != NULL, BLR directly (skip C bridge)
        // Slow path: fall through to CALL_C(xr_jit_call_func)
        case XM_CALL_DIRECT: {
            emit_call_args_from_pool(ctx, ins);
            // Write live PTR regs to spill slots for GC visibility in caller frame
            emit_ptr_spill_writeback(ctx);

            // Record stack map bitmap + emit store safepoint_id
            uint32_t smap_id_direct = record_safepoint(ctx);
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id_direct, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            // Push caller FP to frame stack for GC scanning
            emit_jit_frame_push(ctx);

            // Get nargs value to x17 (for slow path)
            if (!xm_ref_is_none(ins->args[0])) {
                if (xm_ref_is_const(ins->args[0])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[0]);
                    uint64_t val = (uint64_t) ctx->func->consts[ci].val.raw;
                    a64_load_imm64(&ctx->buf, SCRATCH_REG2, val);
                } else {
                    A64Reg arg_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                    a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, arg_reg));
                }
            }

            // --- Inline JIT fast path ---
            // x16 = jit_ctx->call_args[0] (closure ptr)
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_CALL_ARGS_OFFSET));
            // NULL / poison guard: an unconditional dereference of a null
            // or poisoned callee (XR_TAG_NULL or a deopt marker like
            // 0xdead0001dead0001 surviving in call_args[0]) would SIGSEGV
            // at the GC type load below.  xr_jit_call_func performs the
            // same check on its slow path; mirror it here so we route to
            // the slow path and let the VM raise the proper error.
            uint32_t cbz_null_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched to CBZ x16, slow
            a64_buf_emit(&ctx->buf, a64_lsr_imm64(SCRATCH_REG2, SCRATCH_REG, 48));
            uint32_t cbnz_poison_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched to CBNZ x17, slow
            // GC type guard: only XR_TFUNCTION (closures) use fast path.
            // Classes (XR_TCLASS) have different layout and must go to slow path
            // where xr_jit_call_func handles instance allocation + constructor.
            a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, SCRATCH_REG, XM_GC_TYPE_OFFSET));
            a64_buf_emit(&ctx->buf, a64_subs_imm_w(A64_XZR, SCRATCH_REG2, 5));
            uint32_t bne_type_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched to B.NE slow_path

            // Set call_closure for callee's upvalue access
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, JIT_CTX_REG, XM_JIT_CALL_CLOSURE_OFFSET));
            // x16 = closure->proto
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_CLOSURE_PROTO_OFFSET));
            // CBZ x16, slow_path (null proto guard)
            uint32_t cbz_proto_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());

            // Save proto ptr for jit_call_proto
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, JIT_CTX_REG, XM_JIT_CALL_PROTO_OFFSET));

            // x16 = proto->jit_entry
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_PROTO_JIT_ENTRY_OFFSET));
            // CBZ x16, slow_path (no jit_entry → fallback)
            uint32_t cbz_entry_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());

            // Copy call_arg_tags[i+1] → param_tags[i] for JIT→JIT param type
            // pass-through. Uses x17 (SCRATCH_REG2), safe here since x16 holds
            // jit_entry. nargs unknown at compile time, copy all 8 slots.
            for (int pi = 0; pi < 8; pi++) {
                int32_t src = (int32_t) (XM_JIT_CALL_ARG_TAGS_OFFSET + (pi + 1));
                int32_t dst = (int32_t) (XM_JIT_PARAM_TAGS_OFFSET + pi * 8);
                a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG, src));
                a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, dst));
            }
            // Fast path: BLR via call_c_stub
            a64_buf_emit(&ctx->buf,
                         a64_add_imm(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_ARGS_OFFSET + 8));
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;

            // Nested deopt guard: if callee returned DEOPT_MARKER via
            // call_c_stub fast path, redirect to C bridge slow path.
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) XM_DEOPT_MARKER);
            a64_buf_emit(&ctx->buf, a64_cmp(A64_X0, SCRATCH_REG));
            uint32_t beq_cascade_direct = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched: B.EQ → cascade

            // B done (skip slow path)
            uint32_t b_done_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());

            // Cascade handler: callee deopt'd on fast path, clear stale
            // deopt_id (set by callee's deopt stub) before C bridge retry
            uint32_t cascade_direct = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_str_w(A64_XZR, JIT_CTX_REG, XM_JIT_DEOPT_ID_OFFSET));

            // --- Slow path: C bridge fallback ---
            uint32_t slow_path = ctx->buf.count;

            // Restore x17 = nargs (was set earlier, might be clobbered)
            if (!xm_ref_is_none(ins->args[0])) {
                if (xm_ref_is_const(ins->args[0])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[0]);
                    uint64_t val = (uint64_t) ctx->func->consts[ci].val.raw;
                    a64_load_imm64(&ctx->buf, SCRATCH_REG2, val);
                } else {
                    A64Reg arg_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                    a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, arg_reg));
                }
            }

            // x16 = xr_jit_call_func address
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                uint64_t fn_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
                a64_load_imm64(&ctx->buf, SCRATCH_REG, fn_ptr);
            }

            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;

            // --- done label ---
            uint32_t done_label = ctx->buf.count;

            // Patch CBZ null-callee → slow_path
            int32_t off_null = (int32_t) slow_path - (int32_t) cbz_null_idx;
            ctx->buf.code[cbz_null_idx] = a64_cbz(SCRATCH_REG, off_null);

            // Patch CBNZ poison-bits → slow_path
            int32_t off_poison = (int32_t) slow_path - (int32_t) cbnz_poison_idx;
            ctx->buf.code[cbnz_poison_idx] = a64_cbnz(SCRATCH_REG2, off_poison);

            // Patch B.NE type_guard → slow_path
            int32_t off_type = (int32_t) slow_path - (int32_t) bne_type_idx;
            ctx->buf.code[bne_type_idx] = a64_b_cond(A64_CC_NE, off_type);

            // Patch CBZ proto → slow_path
            int32_t off1 = (int32_t) slow_path - (int32_t) cbz_proto_idx;
            ctx->buf.code[cbz_proto_idx] = a64_cbz(SCRATCH_REG, off1);

            // Patch CBZ entry → slow_path
            int32_t off2 = (int32_t) slow_path - (int32_t) cbz_entry_idx;
            ctx->buf.code[cbz_entry_idx] = a64_cbz(SCRATCH_REG, off2);

            // Patch B → done
            int32_t off3 = (int32_t) done_label - (int32_t) b_done_idx;
            ctx->buf.code[b_done_idx] = a64_b(off3);

            // Patch B.EQ cascade → cascade_direct (nested deopt redirect)
            int32_t off_cascade_d = (int32_t) cascade_direct - (int32_t) beq_cascade_direct;
            ctx->buf.code[beq_cascade_direct] = a64_b_cond(A64_CC_EQ, off_cascade_d);

            // Pop frame stack + restore caller's stack map in jit_ctx
            emit_jit_frame_pop(ctx);
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, A64_FP, FRAME_SMAP_PTR_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));

            // x0=payload; tag is in jit_ctx->call_result_tag (set by call_c_stub
            // on both the fast and slow paths through PATCH_CALL_C).
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS) {
                    int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG,
                                                     XM_JIT_CALL_RESULT_TAG_OFFSET));
                    a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag_off));
                }
            }
            if (rd != A64_XZR) {
                bool dst_fp = false;
                if (xm_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XM_REF_INDEX(ins->dst);
                    if (vi < ctx->func->nvreg)
                        dst_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
                }
                if (dst_fp)
                    a64_buf_emit(&ctx->buf, a64_fmov_gp_to_fp(rd, A64_X0));
                else
                    a64_buf_emit(&ctx->buf, a64_mov(rd, A64_X0));
            }
            break;
        }

        // CALL_KNOWN: cross-function direct BL with known callee proto
        // args[0] = const_ptr(callee XrProto*), args[1] = const(nargs)
        case XM_CALL_KNOWN: {
            emit_call_args_from_pool(ctx, ins);
            // Write live PTR regs to spill slots for GC visibility in caller frame
            emit_ptr_spill_writeback(ctx);

            // Record stack map bitmap + emit store safepoint_id
            uint32_t smap_id_known = record_safepoint(ctx);
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id_known, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            // Extract callee proto pointer
            uint64_t callee_proto_ptr = 0;
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                callee_proto_ptr = (uint64_t) ctx->func->consts[ci].val.raw;
            }

            // Extract nargs
            uint64_t nargs_val = 0;
            if (!xm_ref_is_none(ins->args[1])) {
                if (xm_ref_is_const(ins->args[1])) {
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    nargs_val = (uint64_t) ctx->func->consts[ci].val.i64;
                }
            }

            // Save live caller-saved registers
            A64Reg live_gp[15];
            int ngp = xra_live_gp(ctx, live_gp, rd);
            A64Reg live_fp[8];
            int nfp = xra_live_fp(ctx, live_fp);

            int32_t save_frame = (((ngp + nfp) * 8 + 15) & ~15);
            if (save_frame > 0) {
                a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, save_frame));
                int off = 0, i = 0;
                while (i + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_stp(live_gp[i], live_gp[i + 1], A64_SP, off));
                    off += 16;
                    i += 2;
                }
                if (i < ngp) {
                    a64_buf_emit(&ctx->buf, a64_str(live_gp[i], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_str_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
            }

            // Push caller FP to frame stack for GC scanning
            emit_jit_frame_push(ctx);

            // --- Fast path: load proto->jit_entry directly ---
            a64_load_imm64(&ctx->buf, SCRATCH_REG, callee_proto_ptr);
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, JIT_CTX_REG, XM_JIT_CALL_PROTO_OFFSET));
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_PROTO_JIT_ENTRY_OFFSET));

            // CBZ x16, slow_path (callee not yet compiled)
            uint32_t cbz_entry_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());

            // Set call_closure from call_args[0] so callee can access upvalues.
            // Without this, xr_jit_upval_get reads stale call_closure.
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_ARGS_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_CLOSURE_OFFSET));

            // Copy call_arg_tags[i+1] → param_tags[i] for JIT→JIT param type
            // pass-through. Callee prologue reads param_tags to init
            // vreg_runtime_tags for TAGGED params. Uses x17 (SCRATCH_REG2).
            for (uint64_t pi = 0; pi < nargs_val && pi < 8; pi++) {
                int32_t src = (int32_t) (XM_JIT_CALL_ARG_TAGS_OFFSET + (pi + 1));
                int32_t dst = (int32_t) (XM_JIT_PARAM_TAGS_OFFSET + pi * 8);
                a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG, src));
                a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, dst));
            }
            // Fast path: direct BLR to callee's jit_entry
            a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
            a64_buf_emit(&ctx->buf, a64_add_imm(A64_X1, JIT_CTX_REG, XM_JIT_CALL_ARGS_OFFSET + 8));
            a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));
            // Callee sets x1=tag via XM_JMP_RET. Store it to call_result_tag
            // so the done-path tag read is uniform with the slow (call_c_stub) path.
            a64_buf_emit(&ctx->buf, a64_str(A64_X1, JIT_CTX_REG, XM_JIT_CALL_RESULT_TAG_OFFSET));

            // Nested deopt guard: if callee returned DEOPT_MARKER,
            // redirect to C bridge slow path (xr_jit_call_func handles it)
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, (uint64_t) XM_DEOPT_MARKER);
            a64_buf_emit(&ctx->buf, a64_cmp(A64_X0, SCRATCH_REG2));
            uint32_t beq_cascade_known = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched: B.EQ → cascade

            // B done
            uint32_t b_done_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());

            // Cascade handler: clear stale deopt_id before C bridge retry
            uint32_t cascade_known = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_str_w(A64_XZR, JIT_CTX_REG, XM_JIT_DEOPT_ID_OFFSET));

            // --- Slow path: fallback to xr_jit_call_func C bridge ---
            uint32_t slow_path = ctx->buf.count;
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, nargs_val);
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_call_func);
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;

            // --- done label ---
            uint32_t done_label = ctx->buf.count;

            int32_t off_cbz = (int32_t) slow_path - (int32_t) cbz_entry_idx;
            ctx->buf.code[cbz_entry_idx] = a64_cbz(SCRATCH_REG, off_cbz);
            int32_t off_b = (int32_t) done_label - (int32_t) b_done_idx;
            ctx->buf.code[b_done_idx] = a64_b(off_b);

            // Patch B.EQ cascade → cascade_known (nested deopt redirect)
            int32_t off_cascade_k = (int32_t) cascade_known - (int32_t) beq_cascade_known;
            ctx->buf.code[beq_cascade_known] = a64_b_cond(A64_CC_EQ, off_cascade_k);

            // Pop frame stack + restore caller's stack map in jit_ctx
            emit_jit_frame_pop(ctx);
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, A64_FP, FRAME_SMAP_PTR_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));

            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, A64_X0));

            // Save tag from call_result_tag to vreg_runtime_tags[vi].
            // Both fast path (BLR, callee stores x1→call_result_tag above) and
            // slow path (call_c_stub) leave the tag in call_result_tag.
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS) {
                    int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG,
                                                     XM_JIT_CALL_RESULT_TAG_OFFSET));
                    a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag_off));
                }
            }

            // Restore live registers
            if (save_frame > 0) {
                int off = 0, i = 0;
                while (i + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldp(live_gp[i], live_gp[i + 1], A64_SP, off));
                    off += 16;
                    i += 2;
                }
                if (i < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldr(live_gp[i], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_ldr_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
                a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, save_frame));
            }

            if (rd != A64_XZR) {
                bool dst_fp = false;
                if (xm_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XM_REF_INDEX(ins->dst);
                    if (vi < ctx->func->nvreg)
                        dst_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
                }
                if (dst_fp)
                    a64_buf_emit(&ctx->buf, a64_fmov_gp_to_fp(rd, SCRATCH_REG));
                else
                    a64_buf_emit(&ctx->buf, a64_mov(rd, SCRATCH_REG));
            }
            break;
        }

        // CALL_KNOWN_REG: register-passing cross-function call (nargs <= 2)
        // args[0] = param0 Xm ref, args[1] = param1 Xm ref (or NONE)
        case XM_CALL_KNOWN_REG: {
            emit_call_args_from_pool(ctx, ins);
            // Write live PTR regs to spill slots for GC visibility in caller frame
            emit_ptr_spill_writeback(ctx);

            // Record stack map bitmap + emit store safepoint_id
            uint32_t smap_id_kreg = record_safepoint(ctx);
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id_kreg, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            // Determine nargs from args presence
            int nargs_reg = 0;
            A64Reg arg_regs[2] = {A64_XZR, A64_XZR};
            {
                A64Reg scratches[2] = {SCRATCH_REG, SCRATCH_REG2};
                for (int a = 0; a < 2; a++) {
                    if (xm_ref_is_none(ins->args[a]))
                        break;
                    arg_regs[a] = xra_arg(ctx, ins->args[a], scratches[a]);
                    nargs_reg++;
                }
            }

            // Save live caller-saved regs
            A64Reg live_gp[15];
            int ngp = xra_live_gp(ctx, live_gp, rd);
            A64Reg live_fp[8];
            int nfp = xra_live_fp(ctx, live_fp);

            int32_t save_frame = (((ngp + nfp) * 8 + 15) & ~15);
            if (save_frame > 0) {
                a64_buf_emit(&ctx->buf, a64_sub_imm(A64_SP, A64_SP, save_frame));
                int off = 0, si = 0;
                while (si + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_stp(live_gp[si], live_gp[si + 1], A64_SP, off));
                    off += 16;
                    si += 2;
                }
                if (si < ngp) {
                    a64_buf_emit(&ctx->buf, a64_str(live_gp[si], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_str_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
            }

            // Push caller FP to frame stack for GC scanning
            emit_jit_frame_push(ctx);

            // Load callee proto->jit_fast_entry
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_CALL_PROTO_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_PROTO_JIT_FAST_ENTRY_OFFSET));

            // CBZ x16, slow_path
            uint32_t cbz_fast_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());

            // Set call_closure from call_args[0] so callee can access upvalues.
            // Without this, xr_jit_upval_get reads stale call_closure → SIGBUS.
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_ARGS_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_CLOSURE_OFFSET));

            // --- Fast path: register passing, BLR to fast_entry ---
            if (nargs_reg == 1) {
                A64Reg p0 = alloc_regs[0];
                if (arg_regs[0] != p0)
                    a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[0]));
            } else if (nargs_reg == 2) {
                A64Reg p0 = alloc_regs[0];
                A64Reg p1 = alloc_regs[1];
                if (arg_regs[0] == p1 && arg_regs[1] == p0) {
                    a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, arg_regs[0]));
                    a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[1]));
                    a64_buf_emit(&ctx->buf, a64_mov(p1, SCRATCH_REG2));
                } else if (arg_regs[1] == p0) {
                    if (arg_regs[1] != p1)
                        a64_buf_emit(&ctx->buf, a64_mov(p1, arg_regs[1]));
                    if (arg_regs[0] != p0)
                        a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[0]));
                } else {
                    if (arg_regs[0] != p0)
                        a64_buf_emit(&ctx->buf, a64_mov(p0, arg_regs[0]));
                    if (arg_regs[1] != p1)
                        a64_buf_emit(&ctx->buf, a64_mov(p1, arg_regs[1]));
                }
            }
            a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
            a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));

            // Nested deopt guard: if callee returned DEOPT_MARKER,
            // redirect to C bridge slow path (xr_jit_call_func handles it)
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, (uint64_t) XM_DEOPT_MARKER);
            a64_buf_emit(&ctx->buf, a64_cmp(A64_X0, SCRATCH_REG2));
            uint32_t beq_cascade_fast = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched: B.EQ → cascade

            // B done
            uint32_t b_done_fast_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());

            // Cascade handler: clear stale deopt_id before C bridge retry
            uint32_t cascade_fast = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_str_w(A64_XZR, JIT_CTX_REG, XM_JIT_DEOPT_ID_OFFSET));

            // --- Slow path ---
            uint32_t slow_path_fast = ctx->buf.count;
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, (uint64_t) nargs_reg);
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_call_func);
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;

            // --- done label ---
            uint32_t done_fast_label = ctx->buf.count;

            int32_t off_cbz_f = (int32_t) slow_path_fast - (int32_t) cbz_fast_idx;
            ctx->buf.code[cbz_fast_idx] = a64_cbz(SCRATCH_REG, off_cbz_f);
            int32_t off_b_f = (int32_t) done_fast_label - (int32_t) b_done_fast_idx;
            ctx->buf.code[b_done_fast_idx] = a64_b(off_b_f);

            // Patch B.EQ cascade → cascade_fast (nested deopt redirect)
            int32_t off_cascade_f = (int32_t) cascade_fast - (int32_t) beq_cascade_fast;
            ctx->buf.code[beq_cascade_fast] = a64_b_cond(A64_CC_EQ, off_cascade_f);

            // Pop frame stack + restore caller's stack map in jit_ctx
            emit_jit_frame_pop(ctx);
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, A64_FP, FRAME_SMAP_PTR_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(A64_FP, JIT_CTX_REG, XM_JIT_FRAME_SP_OFFSET));

            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, A64_X0));

            // Restore live registers
            if (save_frame > 0) {
                int off = 0, si = 0;
                while (si + 1 < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldp(live_gp[si], live_gp[si + 1], A64_SP, off));
                    off += 16;
                    si += 2;
                }
                if (si < ngp) {
                    a64_buf_emit(&ctx->buf, a64_ldr(live_gp[si], A64_SP, off));
                    off += 8;
                }
                for (int f = 0; f < nfp; f++) {
                    a64_buf_emit(&ctx->buf, a64_ldr_fp(live_fp[f], A64_SP, off));
                    off += 8;
                }
                a64_buf_emit(&ctx->buf, a64_add_imm(A64_SP, A64_SP, save_frame));
            }

            if (rd != A64_XZR) {
                bool dst_fp = false;
                if (xm_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XM_REF_INDEX(ins->dst);
                    if (vi < ctx->func->nvreg)
                        dst_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
                }
                if (dst_fp)
                    a64_buf_emit(&ctx->buf, a64_fmov_gp_to_fp(rd, SCRATCH_REG));
                else
                    a64_buf_emit(&ctx->buf, a64_mov(rd, SCRATCH_REG));
            }
            break;
        }

        default:
            return false;
    }
    return true;
}

#endif  // __aarch64__
