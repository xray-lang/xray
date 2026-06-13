/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_codegen_mem.c - Xm codegen for runtime/memory/guard instructions
 */

#ifdef __aarch64__

#include "xm_codegen_internal.h"
#include "xm_helper_table.h"
#define XM_RUNTIME_STUBS_ENTRIES
#include "xm_runtime_stubs_gen.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "../runtime/gc/xgc_internal.h"

static bool isnull_uses_runtime_tag(CodegenCtx *ctx, XmRef ref, uint32_t *out_vi) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= ctx->func->nvreg || vi >= XR_JIT_MAX_VREG_TAGS)
        return false;
    XmType ct = xm_ref_ctype(ctx->func, ref);
    if (ctx->func->vregs[vi].rep == XR_REP_TAGGED) {
        *out_vi = vi;
        return true;
    }
    if (ct.kind != XM_TK_TAGGED)
        return false;
    XmIns *def = ctx->func->vregs[vi].def;
    if (!def) {
        *out_vi = vi;
        return true;
    }
    switch (def->op) {
        case XM_CALL_C:
        case XM_CALL_KNOWN:
        case XM_CALL_KNOWN_REG:
        case XM_CALL_DIRECT:
        case XM_CALL_SELF_DIRECT:
            *out_vi = vi;
            return true;
        default:
            return false;
    }
}

/* ========== Inlined RC fast path (XM_RETAIN / XM_RELEASE) ========== */
/*
 * The operand is a GC pointer or null (its XrType lowered to XR_REP_PTR; see
 * xi2xm_retain). The 0-based sign-tagged refcount makes the common case a
 * single load + sign test + inc/dec:
 *   retain : rc >= 0 → rc++          (thread-local owner added)
 *   release: rc >  0 → rc--          (still has other owners)
 * Cold cases (rc < 0 atomic/managed/immortal; rc == 0 unique destroy) call
 * xr_jit_rc_{dup,drop}_ptr through the CALL_C stub, passing the pointer as the
 * extra arg. SCRATCH_REG/SCRATCH_REG2 (x16/x17) are the only scratch used; the
 * register allocator never assigns them, so the operand R is x1..x15 or x16,
 * never x17 — making x17 safe as the refcount temp and slow-path arg.
 */
static void a64_emit_rc_ins(CodegenCtx *ctx, XmIns *ins, bool is_release) {
    A64Reg R = xra_arg(ctx, ins->args[0], SCRATCH_REG);

    // CBZ R, done  (null pointer → nothing to do)
    uint32_t cbz_idx = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_nop());  // patched to CBZ R, done

    // LDR w17, [R, #refcount]
    a64_buf_emit(&ctx->buf, a64_ldr_w(SCRATCH_REG2, R, XM_GC_HDR_REFCOUNT_OFFSET));

    uint32_t bcond_idx;
    if (is_release) {
        // SUBS w17, w17, #1  (w17 = rc - 1; flags from the subtraction)
        a64_buf_emit(&ctx->buf, a64_subs_imm_w(SCRATCH_REG2, SCRATCH_REG2, 1));
        // B.LT slow  (rc - 1 < 0 → rc <= 0: unique destroy / atomic / immortal)
        bcond_idx = ctx->buf.count;
        a64_buf_emit(&ctx->buf, a64_nop());
        // fast: STR w17, [R, #refcount]  (store rc - 1)
        a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, R, XM_GC_HDR_REFCOUNT_OFFSET));
    } else {
        // SUBS WZR, w17, #0  (test sign of rc)
        a64_buf_emit(&ctx->buf, a64_subs_imm_w(A64_XZR, SCRATCH_REG2, 0));
        // B.MI slow  (rc < 0: atomic / managed / immortal)
        bcond_idx = ctx->buf.count;
        a64_buf_emit(&ctx->buf, a64_nop());
        // fast: ADD x17, x17, #1; STR w17, [R, #refcount]
        a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG2, SCRATCH_REG2, 1));
        a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, R, XM_GC_HDR_REFCOUNT_OFFSET));
    }

    // B done  (skip slow path)
    uint32_t b_done_idx = ctx->buf.count;
    a64_buf_emit(&ctx->buf, a64_nop());

    // --- slow path: CALL_C to rc_{dup,drop} with the pointer as extra arg ---
    uint32_t slow_idx = ctx->buf.count;
    // MOV x17 = R  (extra arg = pointer; set before x16 = func entry)
    a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, R));
    uintptr_t entry =
        xm_runtime_stub_entry(is_release ? XM_RUNTIME_STUB_rc_drop : XM_RUNTIME_STUB_rc_dup,
                              XM_RUNTIME_STUB_ABI_CALL_C_EXTRA_ARG);
    if (entry == 0) {
        ctx->had_error = true;
        return;
    }
    a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) entry);
    add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
    a64_buf_emit(&ctx->buf, a64_nop());  // patched to BL call_c_stub
    ctx->has_call_c = true;

    // done:
    uint32_t done_idx = ctx->buf.count;

    int32_t cbz_off = a64_patch_offset(ctx, cbz_idx, done_idx, true, "rc cbz");
    ctx->buf.code[cbz_idx] = a64_cbz(R, cbz_off);
    int32_t bcond_off = a64_patch_offset(ctx, bcond_idx, slow_idx, true, "rc bcond");
    ctx->buf.code[bcond_idx] = a64_b_cond(is_release ? A64_CC_LT : A64_CC_MI, bcond_off);
    int32_t bdone_off = a64_patch_offset(ctx, b_done_idx, done_idx, false, "rc done");
    ctx->buf.code[b_done_idx] = a64_b(bdone_off);
}

bool xm_emit_mem_ops(CodegenCtx *ctx, XmIns *ins, A64Reg rd) {
    XR_DCHECK(ctx != NULL, "emit_mem_ops: NULL ctx");
    XR_DCHECK(ins != NULL, "emit_mem_ops: NULL ins");
    switch (ins->op) {
        // Runtime helper: mixed-type binary arithmetic
        // Inline type conversion for known numeric combos (i64+f64, f64+i64)
        case XM_RT_ADD:
        case XM_RT_SUB:
        case XM_RT_MUL:
        case XM_RT_DIV:
        case XM_RT_MOD: {
            uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XM_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            if (xm_ref_is_vreg(ins->args[1])) {
                uint32_t bi = XM_REF_INDEX(ins->args[1]);
                if (bi < ctx->func->nvreg)
                    tb = ctx->func->vregs[bi].rep;
            }

            // Check ctype for operands to detect NUMERIC/TAGGED
            uint8_t tag_a = VTAG_TAGGED, tag_b = VTAG_TAGGED;
            if (xm_ref_is_vreg(ins->args[0])) {
                tag_a = type_kind_to_vtag(xm_ref_ctype(ctx->func, ins->args[0]).kind);
            } else {
                tag_a = VTAG_I64;
            }  // const: not NUMERIC/TAGGED
            if (xm_ref_is_vreg(ins->args[1])) {
                tag_b = type_kind_to_vtag(xm_ref_ctype(ctx->func, ins->args[1]).kind);
            } else {
                tag_b = VTAG_I64;
            }  // const: not NUMERIC/TAGGED

            // Both numeric: inline convert + float op
            if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
                // Get or convert operand A to FP (d30 = scratch FP)
                A64Reg fa;
                if (ta == XR_REP_F64) {
                    fa = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                } else if (ta == XR_REP_I64 && (tag_a == VTAG_NUMERIC || tag_a == VTAG_TAGGED)) {
                    // Numeric union or unknown: might be float bits.
                    // Load saved tag from jit_ctx scratch and branch.
                    A64Reg ga = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                    fa = 30;
                    // Load saved tag
                    a64_buf_emit(&ctx->buf,
                                 a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_TAG_SCRATCH_OFFSET));
                    // fmov d30, x_ga (bit reinterpret: correct if float)
                    a64_buf_emit(&ctx->buf, a64_fmov_gp_to_fp(fa, ga));
                    // cmp tag, #4 (XR_TAG_F64)
                    a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG2, 4));
                    // b.eq .skip_scvtf (tag == F64 means float → fmov was correct)
                    uint32_t patch_idx = ctx->buf.count;
                    a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for b.eq
                    // scvtf d30, x_ga (overwrite: correct if int)
                    a64_buf_emit(&ctx->buf, a64_scvtf(fa, ga));
                    // .skip_scvtf:
                    uint32_t skip_target = ctx->buf.count;
                    int32_t branch_off =
                        a64_patch_offset(ctx, patch_idx, skip_target, true, "rt_op tag.eq A");
                    ctx->buf.code[patch_idx] = a64_b_cond(A64_CC_EQ, branch_off);
                } else {
                    A64Reg ga = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                    fa = 30;  // scratch FP d30
                    a64_buf_emit(&ctx->buf, a64_scvtf(fa, ga));
                }
                // Get or convert operand B to FP (d31 = scratch FP)
                A64Reg fb;
                if (tb == XR_REP_F64) {
                    fb = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                } else if (tb == XR_REP_I64 && (tag_b == VTAG_NUMERIC || tag_b == VTAG_TAGGED)) {
                    A64Reg gb = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                    fb = 31;
                    a64_buf_emit(&ctx->buf,
                                 a64_ldr(SCRATCH_REG, JIT_CTX_REG, XM_JIT_TAG_SCRATCH_OFFSET));
                    a64_buf_emit(&ctx->buf, a64_fmov_gp_to_fp(fb, gb));
                    // cmp tag, #4 (XR_TAG_F64)
                    a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, 4));
                    // b.eq .skip_scvtf (tag == F64 means float → fmov was correct)
                    uint32_t patch_idx = ctx->buf.count;
                    a64_buf_emit(&ctx->buf, a64_nop());
                    a64_buf_emit(&ctx->buf, a64_scvtf(fb, gb));
                    uint32_t skip_target = ctx->buf.count;
                    int32_t branch_off =
                        a64_patch_offset(ctx, patch_idx, skip_target, true, "rt_op tag.eq B");
                    ctx->buf.code[patch_idx] = a64_b_cond(A64_CC_EQ, branch_off);
                } else {
                    A64Reg gb = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                    fb = 31;  // scratch FP d31
                    a64_buf_emit(&ctx->buf, a64_scvtf(fb, gb));
                }
                // Emit float operation → rd (FP register)
                switch (ins->op) {
                    case XM_RT_ADD:
                        a64_buf_emit(&ctx->buf, a64_fadd(rd, fa, fb));
                        break;
                    case XM_RT_SUB:
                        a64_buf_emit(&ctx->buf, a64_fsub(rd, fa, fb));
                        break;
                    case XM_RT_MUL:
                        a64_buf_emit(&ctx->buf, a64_fmul(rd, fa, fb));
                        break;
                    case XM_RT_DIV:
                        a64_buf_emit(&ctx->buf, a64_fdiv(rd, fa, fb));
                        break;
                    case XM_RT_MOD: {
                        // fmod: a - trunc(a/b) * b
                        a64_buf_emit(&ctx->buf, a64_fdiv(30, fa, fb));
                        a64_buf_emit(&ctx->buf, a64_fcvtzs(SCRATCH_REG, 30));
                        a64_buf_emit(&ctx->buf, a64_scvtf(30, SCRATCH_REG));
                        a64_buf_emit(&ctx->buf, a64_fmul(30, 30, fb));
                        a64_buf_emit(&ctx->buf, a64_fsub(rd, fa, 30));
                        break;
                    }
                    default:
                        ctx->had_error = true;
                        xr_log_warning("cg-arm64", "RT_* mem: unreachable float op %u",
                                       (unsigned) ins->op);
                        break;
                }
            } else {
                // Unknown types: deopt
                add_patch(ctx, PATCH_DEOPT, 0, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());
                ctx->has_deopt = true;
            }
            break;
        }

        // Runtime helper: mixed-type unary negate
        case XM_RT_UNM: {
            uint8_t ta = XR_REP_I64;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XM_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            if (ta == XR_REP_F64) {
                A64Reg fa = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_fneg(rd, fa));
            } else if (ta == XR_REP_I64) {
                A64Reg ga = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_scvtf(rd, ga));
                a64_buf_emit(&ctx->buf, a64_fneg(rd, rd));
            } else {
                add_patch(ctx, PATCH_DEOPT, 0, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());
                ctx->has_deopt = true;
            }
            break;
        }

        // Runtime helper: mixed-type comparison (result is i64: 0 or 1)
        case XM_RT_LT:
        case XM_RT_LE:
        case XM_RT_EQ: {
            uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XM_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            if (xm_ref_is_vreg(ins->args[1])) {
                uint32_t bi = XM_REF_INDEX(ins->args[1]);
                if (bi < ctx->func->nvreg)
                    tb = ctx->func->vregs[bi].rep;
            }

            if ((ta == XR_REP_I64 || ta == XR_REP_F64) && (tb == XR_REP_I64 || tb == XR_REP_F64)) {
                // Convert both to FP, FCMP, CSET
                A64Reg fa;
                if (ta == XR_REP_F64) {
                    fa = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                } else {
                    A64Reg ga = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                    fa = 30;
                    a64_buf_emit(&ctx->buf, a64_scvtf(fa, ga));
                }
                A64Reg fb;
                if (tb == XR_REP_F64) {
                    fb = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                } else {
                    A64Reg gb = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                    fb = 31;
                    a64_buf_emit(&ctx->buf, a64_scvtf(fb, gb));
                }
                a64_buf_emit(&ctx->buf, a64_fcmp(fa, fb));
                A64Cond cc;
                if (ins->op == XM_RT_LT)
                    cc = A64_CC_LT;
                else if (ins->op == XM_RT_LE)
                    cc = A64_CC_LE;
                else
                    cc = A64_CC_EQ;
                a64_buf_emit(&ctx->buf, a64_cset(rd, cc));
            } else {
                add_patch(ctx, PATCH_DEOPT, 0, A64_XZR);
                a64_buf_emit(&ctx->buf, a64_nop());
                ctx->has_deopt = true;
            }
            break;
        }

        // Memory
        case XM_LOAD: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            bool load_fp = (ins->rep == XR_REP_F64);
            if (load_fp)
                a64_buf_emit(&ctx->buf, a64_ldr_fp(rd, rn, 0));
            else
                a64_buf_emit(&ctx->buf, a64_ldr(rd, rn, 0));
            break;
        }
        case XM_STORE: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            bool store_fp = false;
            if (xm_ref_is_vreg(ins->args[1])) {
                uint32_t vi = XM_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg)
                    store_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }
            if (store_fp)
                a64_buf_emit(&ctx->buf, a64_str_fp(rm, rn, 0));
            else
                a64_buf_emit(&ctx->buf, a64_str(rm, rn, 0));
            break;
        }

        // LOAD32S: 32-bit sign-extending load from [base + const_offset].
        // Used for loading int32 fields like XrArray.length into a 64-bit reg.
        // Encoding lives in xisa/arch/arm64.isa as arm64.ldrsw; the generated
        // emitter scales offset by 4 internally so we pass the byte-offset.
        case XM_LOAD32S: {
            A64Reg base = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            XR_DCHECK(offset >= 0 && (offset % 4) == 0,
                      "LDRSW: byte offset must be non-negative and 4-byte aligned");
            XR_DCHECK((offset / 4) < 4096, "LDRSW: imm12 field (offset / 4) must fit in 12 bits");
            a64_buf_emit(&ctx->buf, a64_ldrsw(rd, base, offset));
            break;
        }

        // LOAD8Z: 8-bit zero-extending load from [addr] (struct BOOL fields).
        // Encoding lives in xisa/arch/arm64.isa as arm64.ldrb.
        case XM_LOAD8Z: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrb(rd, addr, 0));
            break;
        }

        // LOAD8S: 8-bit sign-extending load from [addr] (for int8 fields).
        // Encoding lives in xisa/arch/arm64.isa as arm64.ldrsb.
        case XM_LOAD8S: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrsb(rd, addr, 0));
            break;
        }

        // STORE8: 8-bit store [addr] = low byte of value.
        // Encoding lives in xisa/arch/arm64.isa as arm64.strb.
        case XM_STORE8: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_strb(val, addr, 0));
            break;
        }

        // LOAD16Z: 16-bit zero-extending load from [addr] (for uint16 fields).
        // Encoding lives in xisa/arch/arm64.isa as arm64.ldrh.
        case XM_LOAD16Z: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrh(rd, addr, 0));
            break;
        }

        // LOAD16S: 16-bit sign-extending load from [addr] (for int16 fields).
        // Encoding lives in xisa/arch/arm64.isa as arm64.ldrsh; the offset is
        // 0 here so the generated scale-by-2 on the imm12 field is a no-op.
        case XM_LOAD16S: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrsh(rd, addr, 0));
            break;
        }

        // STORE16: 16-bit store [addr] = low 16 bits of value.
        // Encoding lives in xisa/arch/arm64.isa as arm64.strh.
        case XM_STORE16: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_strh(val, addr, 0));
            break;
        }

        // LOAD32Z: 32-bit zero-extending load from [addr] (for uint32 fields).
        // Encoding lives in xisa/arch/arm64.isa as arm64.ldr_w.
        case XM_LOAD32Z: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldr_w(rd, addr, 0));
            break;
        }

        // STORE32: 32-bit store [addr] = low 32 bits of value.
        // Encoding lives in xisa/arch/arm64.isa as arm64.str_w.
        case XM_STORE32: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_str_w(val, addr, 0));
            break;
        }

        // LOAD_F32: load 32-bit float from [addr], promote to f64.
        // ARM64: LDR St, [Xn] + FCVT Dd, Ss (single → double).
        // Encodings live in xisa/arch/arm64.isa as arm64.ldr_s / arm64.fcvt_d_s.
        case XM_LOAD_F32: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldr_s(rd, addr, 0));
            a64_buf_emit(&ctx->buf, a64_fcvt_d_s(rd, rd));
            break;
        }

        // STORE_F32: truncate f64 to float, store 32-bit to [addr].
        // ARM64: FCVT Ss, Dd + STR St, [Xn].
        // Encodings live in xisa/arch/arm64.isa as arm64.fcvt_s_d / arm64.str_s.
        // Uses FP register 31 (S31) as a scratch destination for the conversion.
        case XM_STORE_F32: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            A64Reg fp_scratch = (A64Reg) 31;
            a64_buf_emit(&ctx->buf, a64_fcvt_s_d(fp_scratch, val));
            a64_buf_emit(&ctx->buf, a64_str_s(fp_scratch, addr, 0));
            break;
        }

        // GUARD_BOUNDS: deopt if (unsigned)index >= (unsigned)length
        // args[0] = index (i64), args[1] = length (i64), dst = const(deopt_id)
        case XM_GUARD_BOUNDS: {
            A64Reg idx_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg len_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            // CMP idx, len (unsigned comparison)
            a64_buf_emit(&ctx->buf, a64_cmp(idx_reg, len_reg));
            // Load deopt_id into x17
            {
                uint16_t did = 0xFFFF;
                if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                    uint32_t dci = XM_REF_INDEX(ins->dst);
                    did = (uint16_t) ctx->func->consts[dci].val.raw;
                }
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, did, 0));
            }
            // B.CS deopt (carry set = unsigned >=, i.e. out of bounds)
            add_patch(ctx, PATCH_DEOPT_CS, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.CS
            ctx->has_deopt = true;
            break;
        }

        // LOAD_FIELD: load XrValue payload from object field
        // args[0] = obj ptr, args[1] = const(byte_offset)
        // Builder computes byte_offset based on object type:
        //   Instance: sizeof(XrGCHeader) + sizeof(klass*) + idx * sizeof(XrValue)
        //   Json:     sizeof(XrGCHeader) + idx * sizeof(XrValue)
        case XM_LOAD_FIELD: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            // When field type is unknown (Json dynamic fields), save the
            // runtime tag to jit_ctx scratch so rt.add can distinguish
            // int payloads from float bit patterns.
            // MUST happen BEFORE payload load: rd may alias obj register.
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && ins->ctype.kind == XM_TK_UNKNOWN &&
                    ins->rep == XR_REP_I64) {
                    a64_buf_emit(&ctx->buf,
                                 a64_ldrb(SCRATCH_REG2, obj, offset + XM_XRVALUE_TAG_OFFSET));
                    a64_buf_emit(&ctx->buf,
                                 a64_str(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_TAG_SCRATCH_OFFSET));
                }
            }
            // Payload sits at byte 8 within the XrValue struct
            if (ins->rep == XR_REP_F64) {
                a64_buf_emit(&ctx->buf, a64_ldr_fp(rd, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET));
            } else {
                a64_buf_emit(&ctx->buf, a64_ldr(rd, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET));
            }
            break;
        }

        // STORE_FIELD: store complete XrValue (payload + tag) to object field
        // ins->rep  = explicit XrValue tag (0-15), or XM_SF_TAG_RUNTIME
        // ins->dst   = const(byte_offset)
        // args[0]    = obj ptr
        // args[1]    = value to store
        case XM_STORE_FIELD: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                uint32_t ci = XM_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }

            // Determine if payload is F64 (needs FP store instruction)
            bool is_fp = false;
            if (xm_ref_is_vreg(ins->args[1])) {
                uint32_t vi = XM_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg)
                    is_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }

            // Store payload at XrValue byte 8 (payload union)
            if (is_fp) {
                a64_buf_emit(&ctx->buf, a64_str_fp(val, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET));
            } else {
                a64_buf_emit(&ctx->buf, a64_str(val, obj, offset + XM_XRVALUE_PAYLOAD_OFFSET));
            }

            // Store descriptor (tag + flags + heap_type) at XrValue byte 0-3.
            // Merged into a single 32-bit store to minimize instruction count.
            // Layout: [0]=tag, [1]=flags(0), [2-3]=heap_type
            uint8_t xr_tag = ins->rep;
            {
                bool is_ptr_val = false;
                uint32_t tag_val = 0;

                if (xr_tag == XM_SF_TAG_RUNTIME) {
                    // Try vreg semantic tag first, then fall back to machine type.
                    tag_val = XR_TAG_PTR;
                    if (xm_ref_is_vreg(ins->args[1])) {
                        XmType vct = xm_ref_ctype(ctx->func, ins->args[1]);
                        uint8_t vk = type_kind_to_vtag(vct.kind);
                        if (vtag_is_concrete(vk)) {
                            tag_val = vtag_to_value_tag(vk);
                            is_ptr_val = xm_type_is_ptr(vct.kind);
                        } else {
                            // Fall back to machine type inference
                            uint32_t vi = XM_REF_INDEX(ins->args[1]);
                            uint8_t vt =
                                (vi < ctx->func->nvreg) ? ctx->func->vregs[vi].rep : XR_REP_TAGGED;
                            if (vt == XR_REP_F64)
                                tag_val = XR_TAG_F64;
                            else if (vt == XR_REP_I64)
                                tag_val = XR_TAG_I64;
                            is_ptr_val = (vt == XR_REP_PTR || vt == XR_REP_TAGGED);
                        }
                    }
                } else {
                    tag_val = xr_tag;
                    is_ptr_val = (xr_tag == XR_TAG_PTR);
                }

                if (is_ptr_val) {
                    // PTR: read gc_type, build tag|(gc_type<<16), single str_w
                    // SCRATCH_REG2 = gc_type (uint16 from GC header)
                    a64_buf_emit(&ctx->buf, a64_ldrh(SCRATCH_REG2, val, XM_GC_HDR_TYPE_OFFSET));
                    // SCRATCH_REG = tag_val (bits [0..15])
                    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, (uint16_t) tag_val, 0));
                    // MOVK scratch, gc_type, LSL#16 → gc_type in bits [16..31]
                    // Cannot use movk with register; use add_lsl instead:
                    // scratch = tag_val + (gc_type << 16)
                    a64_buf_emit(&ctx->buf,
                                 a64_add_lsl(SCRATCH_REG, SCRATCH_REG, SCRATCH_REG2, 16));
                    a64_buf_emit(&ctx->buf,
                                 a64_str_w(SCRATCH_REG, obj, offset + XM_XRVALUE_TAG_OFFSET));
                } else {
                    // Non-PTR: tag with heap_type=0, single 32-bit store
                    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, (uint16_t) tag_val, 0));
                    a64_buf_emit(&ctx->buf,
                                 a64_str_w(SCRATCH_REG, obj, offset + XM_XRVALUE_TAG_OFFSET));
                }
            }

            // Write barriers are managed by xm_insert_write_barriers pass
            // which inserts XM_BARRIER_FWD after PTR STORE_FIELD ops.
            // Do NOT emit inline barriers here — their cbz skip offsets
            // conflict with the pass-inserted BARRIER_FWD instructions
            // that immediately follow this STORE_FIELD in the code stream.
            break;
        }

        // STORE_CORO: store value to JIT scratch at known byte offset
        // dst = const(byte_offset), args[0] = value to store
        // All STORE_CORO targets are JIT scratch fields (call_args, etc.)
        case XM_STORE_CORO: {
            A64Reg val = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                uint32_t ci = XM_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            // F64 vreg → FP register → use FP store instruction
            bool val_fp = false;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XM_REF_INDEX(ins->args[0]);
                if (vi < ctx->func->nvreg)
                    val_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }
            if (val_fp)
                a64_buf_emit(&ctx->buf, a64_str_fp(val, JIT_CTX_REG, offset));
            else
                a64_buf_emit(&ctx->buf, a64_str(val, JIT_CTX_REG, offset));
            break;
        }

        // STORE_CORO_BYTE: store low byte of value to jit_ctx at known offset
        // dst = const(byte_offset), args[0] = value (low 8 bits written)
        case XM_STORE_CORO_BYTE: {
            A64Reg val = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                uint32_t ci = XM_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_strb(val, JIT_CTX_REG, offset));
            break;
        }

        // LOAD_CORO: load value from jit_ctx at known byte offset
        // args[0] = const(byte_offset), result = i64
        case XM_LOAD_CORO: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[0]) && xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_ldr(rd, JIT_CTX_REG, offset));
            xra_maybe_spill(ctx, ins->dst);
            break;
        }

        // LOAD_CORO_BYTE: load single byte from jit_ctx at known offset
        // args[0] = const(byte_offset), result = i64 (zero-extended)
        case XM_LOAD_CORO_BYTE: {
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[0]) && xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_ldrb(rd, JIT_CTX_REG, offset));
            xra_maybe_spill(ctx, ins->dst);
            break;
        }

        // TAG_LOAD: load tag field from XrValue in memory
        // args[0] = ptr to object, args[1] = const(byte_offset_to_tag)
        // Loads the 8-bit tag at the given byte offset
        // Builder computes: field_payload_offset + XM_XRVALUE_TAG_OFFSET
        case XM_TAG_LOAD: {
            A64Reg ptr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (!xm_ref_is_none(ins->args[1]) && xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_ldrb(rd, ptr, offset));
            break;
        }

        // TAG_CHECK: check tag == expected, deopt if mismatch
        // Same semantics as GUARD_TAG
        case XM_TAG_CHECK: {
            A64Reg val_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG, val_reg, XM_XRVALUE_TAG_OFFSET));
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                uint32_t expected = (uint32_t) ctx->func->consts[ci].val.raw;
                a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, expected & 0xFFF));
            } else {
                A64Reg exp_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG, exp_reg));
            }
            add_patch(ctx, PATCH_DEOPT_NE, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_deopt = true;
            break;
        }

        // GUARD_CLASS: check shape_id in GC header matches expected
        // args[0] = obj ptr, args[1] = const(expected_shape_id)
        // XrGCHeader.extra (uint16) at offset XM_GC_EXTRA_OFFSET stores shape_id
        case XM_GUARD_CLASS: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrh(SCRATCH_REG, obj, XM_GC_EXTRA_OFFSET));
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                uint32_t expected = (uint32_t) ctx->func->consts[ci].val.raw;
                a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, expected & 0xFFF));
            } else {
                A64Reg exp_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG, exp_reg));
            }
            add_patch(ctx, PATCH_DEOPT_NE, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_deopt = true;
            break;
        }

        // GUARD_KLASS: check inst->klass pointer matches expected XrClass*
        // args[0] = instance ptr, args[1] = const_ptr(expected_klass)
        case XM_GUARD_KLASS: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // Load klass pointer from XrInstance (offset 16 after GCHeader)
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, obj, XM_INSTANCE_KLASS_OFFSET));
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                int64_t expected = ctx->func->consts[ci].val.i64;
                // Load expected klass pointer into SCRATCH_REG2
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, expected & 0xFFFF, 0));
                a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG2, (expected >> 16) & 0xFFFF, 16));
                a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG2, (expected >> 32) & 0xFFFF, 32));
                a64_buf_emit(&ctx->buf, a64_movk(SCRATCH_REG2, (expected >> 48) & 0xFFFF, 48));
                a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG, SCRATCH_REG2));
            } else {
                A64Reg exp_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG, exp_reg));
            }
            add_patch(ctx, PATCH_DEOPT_NE, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_deopt = true;
            break;
        }

        // CATCH: load exception value from jit_ctx->exception, then clear it
        case XM_CATCH: {
            a64_buf_emit(&ctx->buf, a64_ldr(rd, JIT_CTX_REG, XM_JIT_EXCEPTION_OFFSET));
            // Clear exception = NULL
            a64_buf_emit(&ctx->buf, a64_str(A64_XZR, JIT_CTX_REG, XM_JIT_EXCEPTION_OFFSET));
            break;
        }

        // THROW: handled by CALL_C to xr_jit_throw (builder emits CALL_C + GOTO/RET)
        // No separate codegen needed — XM_THROW is never emitted directly

        // ALLOC: inline bump-pointer fast path + CALL_C slow path
        // args[0] = const(gc_type), args[1] = const(aligned_total_size)
        // Result: XrGCHeader* in rd
        //
        // Fast path (~18 instructions):
        //   1. Load gc = coro->coro_gc
        //   2. Bump check: cursor + size <= limit
        //   3. Commit: gc->cursor = new_cursor
        //   4. Init GC header inline (type, extra, refcount, objsize)
        //   5. Skip slow path
        // Slow path (~7 instructions):
        //   CALL_C to xr_jit_alloc(coro, type<<32|size)
        case XM_ALLOC: {
            // args[0] = packed constant: (gc_extra << 8) | gc_type
            // args[1] = allocation size in bytes
            uint8_t gc_type = 0;
            uint16_t gc_extra = 0;
            uint32_t alloc_size = 0;
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                int64_t packed = ctx->func->consts[ci].val.i64;
                gc_type = (uint8_t) (packed & 0xFF);
                gc_extra = (uint16_t) ((packed >> 8) & 0xFFFF);
            }
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                alloc_size = (uint32_t) ctx->func->consts[ci].val.i64;
            }
            alloc_size = (alloc_size + 7) & ~7u;
            bool force_slow_path = xr_gc_type_may_need_finalize(gc_type);
            uint32_t force_slow_idx = 0;
            if (force_slow_path) {
                force_slow_idx = ctx->buf.count;
                a64_buf_emit(&ctx->buf, a64_nop());  // patched below
            }

            // --- Fast path: inline bump-pointer ---
            // LDR x16, [x19, #XM_CORO_GC_OFFSET]  — gc = coro->coro_gc
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, CORO_REG, XM_CORO_GC_OFFSET));
            // CBZ x16, slow_path  (gc == NULL → slow path)
            uint32_t cbz_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_cbz(SCRATCH_REG, 0));  // patched below
            // LDR x17, [x16, #0]  — cursor = gc->immix.cursor
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, SCRATCH_REG, XM_IMMIX_CURSOR_OFFSET));
            // ADD x17, x17, #size  — new_cursor = cursor + size
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG2, SCRATCH_REG2, alloc_size));
            // LDR rd, [x16, #8]  — limit (borrow rd temporarily)
            a64_buf_emit(&ctx->buf, a64_ldr(rd, SCRATCH_REG, XM_IMMIX_LIMIT_OFFSET));
            // CMP x17, rd  — new_cursor <= limit?
            a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG2, rd));
            // B.HI slow_path
            uint32_t bhi_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched below

            // Commit: STR x17, [x16, #0]  — gc->cursor = new_cursor
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, SCRATCH_REG, XM_IMMIX_CURSOR_OFFSET));
            // SUB rd, x17, #size  — rd = allocated GCHeader*
            a64_buf_emit(&ctx->buf, a64_sub_imm(rd, SCRATCH_REG2, alloc_size));

            // Init GC header inline:
            // MOV w17, #gc_type
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, gc_type, 0));
            // STRH w17, [rd, #type]  — type
            a64_buf_emit(&ctx->buf, a64_strh(SCRATCH_REG2, rd, XM_GC_HDR_TYPE_OFFSET));
            // STRH extra, [rd, #extra]  — extra (contains shape_id for Json objects)
            if (gc_extra != 0) {
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, gc_extra, 0));
                a64_buf_emit(&ctx->buf, a64_strh(SCRATCH_REG, rd, XM_GC_HDR_EXTRA_OFFSET));
            } else {
                a64_buf_emit(&ctx->buf, a64_strh(A64_XZR, rd, XM_GC_HDR_EXTRA_OFFSET));
            }
            // STR wzr, [rd, #refcount]  — refcount = 0 (RC is 0-based: unique == 0)
            a64_buf_emit(&ctx->buf, a64_str_w(A64_XZR, rd, XM_GC_HDR_REFCOUNT_OFFSET));
            // MOV w16, #alloc_size; STR w16, [rd, #objsize]  — objsize
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, alloc_size & 0xFFFF, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, rd, XM_GC_HDR_OBJSIZE_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str_w(A64_XZR, rd, XM_GC_HDR_RSV_OFFSET));

            // --- Inline alloc_post: GC bookkeeping without CALL_C stub ---
            // alloc_marks are DEFERRED: xr_immix_flush_marks() at slow path
            // entry marks all lines from mark_cursor to cursor in one batch.

            // 1. Block allocation accounting.
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, ~(uint64_t) XM_IMMIX_BLOCK_SIZE_MASK);
            a64_buf_emit(&ctx->buf, a64_and(SCRATCH_REG2, rd, SCRATCH_REG2));  // x17 = block
            a64_buf_emit(&ctx->buf,
                         a64_ldr_w(SCRATCH_REG, SCRATCH_REG2, XM_IMMIX_BLOCK_ALLOC_COUNT_OFFSET));
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, 1));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG, SCRATCH_REG2, XM_IMMIX_BLOCK_ALLOC_COUNT_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG, SCRATCH_REG2, XM_IMMIX_BLOCK_ALLOC_BYTES_OFFSET));
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, alloc_size));
            a64_buf_emit(&ctx->buf,
                         a64_str(SCRATCH_REG, SCRATCH_REG2, XM_IMMIX_BLOCK_ALLOC_BYTES_OFFSET));

            // 2. GC stats: totalbytes += size (RC has no GCdebt/collection
            //    trigger; only the bytes counter is kept for gc.info()).
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG2, CORO_REG, XM_CORO_GC_OFFSET));  // x17 = gc
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG2, XM_GC_TOTALBYTES_OFFSET));
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, alloc_size));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, SCRATCH_REG2, XM_GC_TOTALBYTES_OFFSET));

            // B alloc_done (skip slow path)
            uint32_t b_done_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched below

            // --- Slow path: CALL_C to xr_jit_alloc ---
            uint32_t slow_path_idx = ctx->buf.count;

            // Patch CBZ and B.HI to point here
            if (force_slow_path) {
                int32_t force_off =
                    a64_patch_offset(ctx, force_slow_idx, slow_path_idx, false, "alloc force slow");
                ctx->buf.code[force_slow_idx] = a64_b(force_off);
            }
            int32_t cbz_off = a64_patch_offset(ctx, cbz_idx, slow_path_idx, true, "alloc CBZ");
            ctx->buf.code[cbz_idx] = a64_cbz(SCRATCH_REG, cbz_off);
            int32_t bhi_off = a64_patch_offset(ctx, bhi_idx, slow_path_idx, true, "alloc B.HI");
            ctx->buf.code[bhi_idx] = a64_b_cond(A64_CC_HI, bhi_off);

            // Load func ptr and packed arg
            uintptr_t alloc_entry =
                xm_runtime_stub_entry(XM_RUNTIME_STUB_alloc, XM_RUNTIME_STUB_ABI_CALL_C_EXTRA_ARG);
            if (alloc_entry == 0) {
                ctx->had_error = true;
                return true;
            }
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) alloc_entry);
            uint64_t packed = ((uint64_t) gc_type << 32) | (uint64_t) alloc_size;
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, packed);
            // BL call_c_stub
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;
            // MOV rd, x0  — result from slow path
            if (rd != A64_X0) {
                a64_buf_emit(&ctx->buf, a64_mov(rd, A64_X0));
            }

            // NULL check: if allocation failed, deopt back to interpreter
            add_patch(ctx, PATCH_DEOPT_CBZ, 0, rd);
            a64_buf_emit(&ctx->buf, a64_nop());  // patched to CBZ rd, deopt
            ctx->has_deopt = true;

            // Set gc_extra after slow path: xr_coro_gc_newobj sets extra=0,
            // but we need shape_id and other metadata encoded in gc_extra.
            if (gc_extra != 0) {
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, gc_extra, 0));
                a64_buf_emit(&ctx->buf, a64_strh(SCRATCH_REG, rd, XM_GC_HDR_EXTRA_OFFSET));
            }

            // Patch B to alloc_done
            uint32_t done_idx = ctx->buf.count;
            int32_t b_done_off = a64_patch_offset(ctx, b_done_idx, done_idx, false, "alloc done");
            ctx->buf.code[b_done_idx] = a64_b(b_done_off);

            break;
        }

        case XM_RETAIN:
            a64_emit_rc_ins(ctx, ins, false);
            break;

        case XM_RELEASE:
            a64_emit_rc_ins(ctx, ins, true);
            break;

        case XM_NOP:
            a64_buf_emit(&ctx->buf, a64_nop());
            break;

        // Safepoint: guard page poll.
        //
        // WHY THIS DESIGN:
        //   Single LDR instruction per back-edge. x20 holds guard page address.
        //   Normal: page PROT_READ, LDR succeeds with zero overhead.
        //   Armed: page PROT_NONE, LDR faults → SIGSEGV → trampoline →
        //   safepoint work (GC, cancel, heartbeat) → disarm → resume.
        //   Sysmon periodically re-arms via mprotect every ~2ms.
        case XM_SAFEPOINT: {
            record_safepoint(ctx);

            // Guard page poll: faults when page is armed (PROT_NONE)
            a64_buf_emit(&ctx->buf, a64_ldr_w(A64_XZR, SAFEPT_PAGE_REG, 0));

            ctx->has_safepoints = true;
            break;
        }

        // Write barriers: move args to scratch regs, BL to shared stub
        case XM_BARRIER_FWD: {
            A64Reg parent_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg child_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, parent_reg));
            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, child_reg));
            a64_buf_emit(&ctx->buf, a64_cbz(CORO_REG, 2));
            add_patch(ctx, PATCH_BARRIER_FWD, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_barriers = true;
            break;
        }
        case XM_BARRIER_BACK: {
            A64Reg container_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG, container_reg));
            a64_buf_emit(&ctx->buf, a64_cbz(CORO_REG, 2));
            add_patch(ctx, PATCH_BARRIER_BACK, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_barriers = true;
            break;
        }

        // Guard: check tag == expected, deopt if mismatch
        // args[0] = tagged value ptr, args[1] = expected tag (const i64)
        case XM_GUARD_TAG: {
            A64Reg val_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // Load tag field: LDRB w16, [val_reg, #0] (XrValue.tag at byte 0)
            a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG, val_reg, XM_XRVALUE_TAG_OFFSET));
            // Load expected tag
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                uint32_t expected = (uint32_t) ctx->func->consts[ci].val.raw;
                a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, expected & 0xFFF));
            } else {
                A64Reg exp_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG, exp_reg));
            }
            // Load deopt_id into x17 for the deopt stub
            {
                uint16_t did = 0xFFFF;
                if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                    uint32_t dci = XM_REF_INDEX(ins->dst);
                    did = (uint16_t) ctx->func->consts[dci].val.raw;
                }
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, did, 0));
            }
            // B.NE deopt_stub (patched later)
            add_patch(ctx, PATCH_DEOPT_NE, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.NE
            ctx->has_deopt = true;
            break;
        }

        // Explicit deopt point (unconditional)
        case XM_DEOPT: {
            // Load deopt_id into x17
            {
                uint16_t did = 0xFFFF;
                if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                    uint32_t dci = XM_REF_INDEX(ins->dst);
                    did = (uint16_t) ctx->func->consts[dci].val.raw;
                }
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, did, 0));
            }
            add_patch(ctx, PATCH_DEOPT, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_deopt = true;
            break;
        }

        // Guard non-null: deopt if value is zero/null
        case XM_GUARD_NONNULL: {
            A64Reg val_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // CBZ val_reg → deopt (but CBZ is forward branch, we use CMP+B.EQ)
            a64_buf_emit(&ctx->buf, a64_cmp_imm(val_reg, 0));
            // Load deopt_id into x17
            {
                uint16_t did = 0xFFFF;
                if (!xm_ref_is_none(ins->dst) && xm_ref_is_const(ins->dst)) {
                    uint32_t dci = XM_REF_INDEX(ins->dst);
                    did = (uint16_t) ctx->func->consts[dci].val.raw;
                }
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, did, 0));
            }
            add_patch(ctx, PATCH_DEOPT_EQ, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.EQ
            ctx->has_deopt = true;
            break;
        }

        // RT_ARRAY_NEW: create new array with given capacity via C helper
        // args[0] = capacity (const or vreg i64)
        // Result: ptr to XrArray in rd
        case XM_RT_ARRAY_NEW: {
            // Load helper address to x16
            a64_load_imm64(&ctx->buf, SCRATCH_REG,
                           (uint64_t) (uintptr_t) xm_helper_func(XM_HELPER_rt_array_new));
            // Load capacity to x17
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                uint64_t cap = (uint64_t) ctx->func->consts[ci].val.i64;
                a64_load_imm64(&ctx->buf, SCRATCH_REG2, cap);
            } else {
                A64Reg cap_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, cap_reg));
            }
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;
            if (rd != A64_XZR)
                a64_buf_emit(&ctx->buf, a64_mov(rd, A64_X0));
            break;
        }

        // RT_ARRAY_PUSH: push value to array via C helper
        // args[0] = arr (vreg PTR), args[1] = val (vreg)
        case XM_RT_ARRAY_PUSH: {
            // Store arr to call_args[0]
            A64Reg arr_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_str(arr_reg, JIT_CTX_REG, 0));
            // Store val to call_args[1]
            A64Reg val_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            bool val_fp = false;
            if (xm_ref_is_vreg(ins->args[1])) {
                uint32_t vi = XM_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg)
                    val_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }
            if (val_fp)
                a64_buf_emit(&ctx->buf, a64_str_fp(val_reg, JIT_CTX_REG, 8));
            else
                a64_buf_emit(&ctx->buf, a64_str(val_reg, JIT_CTX_REG, 8));
            // Determine val value_tag for reconstruction.
            // If vreg has a known concrete vtag, convert to value_tag.
            // Otherwise 0xFF = UNKNOWN, patched at runtime from vreg_runtime_tags.
            uint8_t val_tag = 0xFF;  // unknown
            if (xm_ref_is_vreg(ins->args[1])) {
                XmType vct = xm_ref_ctype(ctx->func, ins->args[1]);
                uint8_t vk = type_kind_to_vtag(vct.kind);
                if (vk != VTAG_TAGGED && vk != VTAG_NUMERIC) {
                    uint8_t vval = vtag_to_value_tag(vk);
                    if (vval != 0xFF)
                        val_tag = vval;
                }
            }
            // Write val_tag to call_arg_tags[1] (helper reads it via call_arg_tags)
            int32_t tag1_off = (int32_t) XM_JIT_CALL_ARG_TAGS_OFFSET + 1;
            if (val_tag == 0xFF && xm_ref_is_vreg(ins->args[1])) {
                // Dynamic patch: load from vreg_runtime_tags[vi]
                uint32_t vi = XM_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS) {
                    int32_t src_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, JIT_CTX_REG, src_off));
                    a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag1_off));
                } else {
                    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, val_tag, 0));
                    a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag1_off));
                }
            } else {
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, val_tag, 0));
                a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag1_off));
            }
            a64_load_imm64(&ctx->buf, SCRATCH_REG,
                           (uint64_t) (uintptr_t) xm_helper_func(XM_HELPER_rt_array_push));
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, (uint64_t) val_tag);
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;
            break;
        }

        // RT_ARRAY_LEN: get array length via C helper
        // args[0] = arr (vreg PTR)
        // Result: i64 length in rd
        case XM_RT_ARRAY_LEN: {
            A64Reg arr_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_str(arr_reg, JIT_CTX_REG, 0));
            a64_load_imm64(&ctx->buf, SCRATCH_REG,
                           (uint64_t) (uintptr_t) xm_helper_func(XM_HELPER_rt_array_len));
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, 0);
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;
            if (rd != A64_XZR)
                a64_buf_emit(&ctx->buf, a64_mov(rd, A64_X0));
            break;
        }

        // RT_MAP_NEW: create new map via C helper
        // args[0] = capacity (const or vreg i64)
        // Result: ptr to XrMap in rd
        case XM_RT_MAP_NEW: {
            a64_load_imm64(&ctx->buf, SCRATCH_REG,
                           (uint64_t) (uintptr_t) xm_helper_func(XM_HELPER_rt_map_new));
            if (xm_ref_is_const(ins->args[0])) {
                uint32_t ci = XM_REF_INDEX(ins->args[0]);
                uint64_t cap = (uint64_t) ctx->func->consts[ci].val.i64;
                a64_load_imm64(&ctx->buf, SCRATCH_REG2, cap);
            } else {
                A64Reg cap_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
                a64_buf_emit(&ctx->buf, a64_mov(SCRATCH_REG2, cap_reg));
            }
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;
            if (rd != A64_XZR)
                a64_buf_emit(&ctx->buf, a64_mov(rd, A64_X0));
            break;
        }

        // RT_ISNULL: check if value is null (tag == 0)
        // args[0] = value (vreg), result: i64 (0 or 1)
        case XM_RT_ISNULL: {
            if (xm_ref_is_vreg(ins->args[0])) {
                XmType ct = xm_ref_ctype(ctx->func, ins->args[0]);
                if (ct.kind == XM_TK_NULL) {
                    a64_buf_emit(&ctx->buf, a64_movz(rd, 1, 0));
                    break;
                }
                if (ct.kind == XM_TK_INT || ct.kind == XM_TK_FLOAT || ct.kind == XM_TK_BOOL) {
                    a64_buf_emit(&ctx->buf, a64_movz(rd, 0, 0));
                    break;
                }
                uint32_t vi = 0;
                if (isnull_uses_runtime_tag(ctx, ins->args[0], &vi)) {
                    int32_t tag_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                    a64_buf_emit(&ctx->buf, a64_ldrb(rd, JIT_CTX_REG, tag_off));
                    a64_buf_emit(&ctx->buf, a64_cmp_imm(rd, XR_TAG_NULL));
                    a64_buf_emit(&ctx->buf, a64_cset(rd, A64_CC_EQ));
                    break;
                }
            }
            A64Reg val = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_cmp_imm(val, 0));
            a64_buf_emit(&ctx->buf, a64_cset(rd, A64_CC_EQ));
            break;
        }

        // RT_INDEX_GET / RT_INDEX_SET: delegate to existing CALL_C helpers
        // These shouldn't normally be emitted by the builder in JIT mode,
        // but handle gracefully by falling through to default with NOP.
        case XM_RT_INDEX_GET:
        case XM_RT_INDEX_SET:
        case XM_RT_PRINT: {
            /* Handled via CALL_C in the builder; reaching codegen means
             * the builder did not convert this op.  Emit NOP so the
             * instruction stream stays aligned; the VM fallback path
             * still handles the actual print. */
            xr_log_debug("a64-cg", "RT opcode %d fell through to NOP (expected CALL_C)", ins->op);
            a64_buf_emit(&ctx->buf, a64_nop());
            break;
        }

        // JIT CPS suspend sequence (AWAIT / CHAN_SEND / CHAN_RECV)
        // Saves all allocatable GP registers, calls await_block helper,
        // returns SUSPEND_MARKER if blocked, or inline-resumes with result.
        case XM_SUSPEND: {
            // Get suspend_id from vreg metadata (stored by builder)
            uint32_t suspend_id = 0;
            if (xm_ref_is_vreg(ins->dst)) {
                uint32_t vi = XM_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg)
                    suspend_id = ctx->func->vregs[vi].call_arg_start;
            }

            // Get discard_result from args[1] (const)
            int64_t discard_result = 0;
            if (xm_ref_is_const(ins->args[1])) {
                uint32_t ci = XM_REF_INDEX(ins->args[1]);
                discard_result = ctx->func->consts[ci].val.i64;
            }

            // 1. Record safepoint bitmap for GC
            uint32_t smap_id = record_safepoint(ctx);

            // 2. Load suspend_state pointer: x16 = jit_state->suspend
            a64_emit_load_jit_state(ctx, SCRATCH_REG);
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_JIT_STATE_SUSPEND_PTR_OFFSET));
            if (suspend_id >= XM_MAX_SUSPEND_ENTRIES) {
                ctx->had_error = true;
                break;
            }

            // 3. Save x1-x15 to suspend_regs[0..14]
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X1, A64_X2, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X3, A64_X4, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 16));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X5, A64_X6, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 32));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X7, A64_X8, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 48));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X9, A64_X10, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 64));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X11, A64_X12, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 80));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X13, A64_X14, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 96));
            a64_buf_emit(&ctx->buf,
                         a64_str(A64_X15, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 112));

            // 4. Save x20-x27 to suspend_regs[15..22]
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X20, A64_X21, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X22, A64_X23, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF + 16));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X24, A64_X25, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF + 32));
            a64_buf_emit(&ctx->buf,
                         a64_stp(A64_X26, A64_X27, SCRATCH_REG, XM_SUSPEND_CALLEE_SAVED_OFF + 48));

            // 4b. Save spill slots to suspend_state.spill[0..nspill-1].
            // The resume entry creates a NEW stack frame whose spill area
            // is uninitialized; these values bridge the old and new frames.
            {
                uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
                if (ns > XM_SUSPEND_SPILL_MAX)
                    ns = XM_SUSPEND_SPILL_MAX;
                for (uint32_t s = 0; s < ns; s++) {
                    int32_t frame_off = SPILL_BASE + (int32_t) s * 8;
                    int32_t regs_off = XM_SUSPEND_SPILL_OFF + (int32_t) s * 8;
                    // LDR x17, [SP, #frame_off]
                    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, A64_SP, frame_off));
                    // STR x17, [x16, #regs_off]  (x16 = suspend_regs base)
                    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, SCRATCH_REG, regs_off));
                }
            }

            // 5. Store suspend_id and smap_id
            a64_emit_load_jit_state(ctx, SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) suspend_id, 0));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_SUSPEND_ID_OFFSET));
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id, 0));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_SUSPEND_SMAP_OFFSET));
            // Update frame + jit_ctx smap for GC during blocked state
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_ACTIVE_SMAP_ID_OFFSET));

            // 6. Pre-store resume info BEFORE block helper (gopark pattern).
            // Once block_helper sets BLOCKED under lock, another worker may
            // wake and resume this coro immediately. resume_entry/proto must
            // already be valid at that point.
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XM_JIT_CALL_PROTO_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_RESUME_PROTO_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG2, SCRATCH_REG2, XM_PROTO_JIT_RESUME_ENTRY_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str(SCRATCH_REG2, SCRATCH_REG, XM_JIT_STATE_RESUME_ENTRY_OFFSET));

            // 7. Call block helper(coro, extra_arg)
            // Block helper selection: func metadata takes priority over default.
            // Channel ops store their helper in suspend_block_helpers[suspend_id].
            // AWAIT leaves it NULL → use default xr_jit_await_block.
            void *block_helper = ctx->func->suspend_block_helpers[suspend_id];
            int64_t helper_extra_arg = 0;
            if (!block_helper) {
                block_helper = xm_helper_func(XM_HELPER_await_block);
                helper_extra_arg = discard_result;
            }
            a64_buf_emit(&ctx->buf, a64_mov(A64_X0, CORO_REG));
            a64_load_imm64(&ctx->buf, A64_X1, (uint64_t) helper_extra_arg);
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) block_helper);
            a64_buf_emit(&ctx->buf, a64_blr(SCRATCH_REG));

            // 8. Check result: x0 = 0 → blocked, x0 != 0 → inline resume
            uint32_t cbnz_not_blocked = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // CBNZ placeholder

            // === Blocked path: return SUSPEND_MARKER ===
            // resume_entry/proto already stored above.
            // Do NOT write to jit_ctx after block_helper — coro may have
            // already been resumed by another worker (gopark race).

            // Load SUSPEND_MARKER (0xDEAD0002DEAD0002) into x0
            a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XM_SUSPEND_MARKER);
            a64_buf_emit(&ctx->buf, a64_movz(A64_X1, 0, 0));  // tag = 0

            // Standard epilogue: restores all callee-saved registers and
            // deallocates the frame via the global frame_patch system.
            // Safe because normal prologue and resume entry use the same
            // frame_size (same function, same nspill). Restoring x28
            // (JIT_CTX_REG) is correct — it restores the C caller's
            // original x28, not the JIT's jit_ctx pointer.
            emit_epilogue(ctx);
            a64_buf_emit(&ctx->buf, a64_ret());

            // === Not-blocked path (inline resume): reload regs + load result ===
            {
                uint32_t here = ctx->buf.count;
                int32_t off = a64_patch_offset(ctx, cbnz_not_blocked, here, true, "suspend CBNZ");
                ctx->buf.code[cbnz_not_blocked] = a64_cbnz(A64_X0, off);
            }

            // Reload suspend pointer (x16 clobbered by BLR)
            a64_emit_load_jit_state(ctx, SCRATCH_REG);
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG, SCRATCH_REG, XM_JIT_STATE_SUSPEND_PTR_OFFSET));

            // Reload x1-x15 from suspend_regs (x20-x27 survived as callee-saved)
            a64_buf_emit(&ctx->buf,
                         a64_ldp(A64_X1, A64_X2, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF));
            a64_buf_emit(&ctx->buf,
                         a64_ldp(A64_X3, A64_X4, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 16));
            a64_buf_emit(&ctx->buf,
                         a64_ldp(A64_X5, A64_X6, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 32));
            a64_buf_emit(&ctx->buf,
                         a64_ldp(A64_X7, A64_X8, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 48));
            a64_buf_emit(&ctx->buf,
                         a64_ldp(A64_X9, A64_X10, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 64));
            a64_buf_emit(&ctx->buf,
                         a64_ldp(A64_X11, A64_X12, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 80));
            a64_buf_emit(&ctx->buf,
                         a64_ldp(A64_X13, A64_X14, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 96));
            a64_buf_emit(&ctx->buf,
                         a64_ldr(A64_X15, SCRATCH_REG, XM_SUSPEND_CALLER_SAVED_OFF + 112));

            // Load await/channel result from suspend_state.result into dst register
            if (rd != A64_XZR) {
                a64_buf_emit(&ctx->buf, a64_ldr(rd, SCRATCH_REG, XM_SUSPEND_RESULT_OFF));
            }

            // Load result_tag from suspend_state and write to vreg_runtime_tags[vi].
            // This ensures downstream CALL_C helpers (e.g. xr_jit_rt_add) get the
            // correct type tag for the await/channel result after resume.
            {
                int32_t res_vreg_off = -1;
                int16_t res_bc_slot = -1;
                if (xm_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XM_REF_INDEX(ins->dst);
                    if (vi < ctx->func->nvreg && vi < XR_JIT_MAX_VREG_TAGS)
                        res_vreg_off = (int32_t) XM_JIT_VREG_RUNTIME_TAGS_OFFSET + (int32_t) vi;
                    if (vi < ctx->func->nvreg)
                        res_bc_slot = ctx->func->vregs[vi].bc_slot;
                }
                if (res_vreg_off >= 0) {
                    // LDRB w17, [x16, #result_tag_off]
                    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, SCRATCH_REG,
                                                     (int32_t) XM_SUSPEND_RESULT_TAG_OFF));
                    // STRB w17, [x28, #vreg_runtime_tags + vi]
                    a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, res_vreg_off));
                }
                // Record for resume entry trampoline
                if (suspend_id < XM_MAX_SUSPEND_ENTRIES) {
                    ctx->suspend_result_bc_slots[suspend_id] = res_bc_slot;
                    ctx->suspend_result_tag_offs[suspend_id] = res_vreg_off;
                }
            }

            // Record continuation point for resume entry jump table
            if (suspend_id < XM_MAX_SUSPEND_ENTRIES) {
                ctx->suspend_cont_offsets[suspend_id] = ctx->buf.count;
                ctx->suspend_smap_ids[suspend_id] = smap_id;
                ctx->suspend_result_regs[suspend_id] = (uint8_t) rd;
                if (suspend_id >= ctx->nsuspend)
                    ctx->nsuspend = suspend_id + 1;
            }
            break;
        }

        default:
            return false;
    }
    return true;
}

#endif  // __aarch64__
