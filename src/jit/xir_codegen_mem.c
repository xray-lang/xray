/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_codegen_mem.c - XIR codegen for runtime/memory/guard instructions
 */

#ifdef __aarch64__

#include "xir_codegen_internal.h"
#include "../base/xchecks.h"

bool xir_emit_mem_ops(CodegenCtx *ctx, XirIns *ins, A64Reg rd) {
    XR_DCHECK(ctx != NULL, "emit_mem_ops: NULL ctx");
    XR_DCHECK(ins != NULL, "emit_mem_ops: NULL ins");
    switch (ins->op) {
        // Runtime helper: mixed-type binary arithmetic
        // Inline type conversion for known numeric combos (i64+f64, f64+i64)
        case XIR_RT_ADD:
        case XIR_RT_SUB:
        case XIR_RT_MUL:
        case XIR_RT_DIV:
        case XIR_RT_MOD: {
            uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t bi = XIR_REF_INDEX(ins->args[1]);
                if (bi < ctx->func->nvreg)
                    tb = ctx->func->vregs[bi].rep;
            }

            // Check ctype for operands to detect NUMERIC/TAGGED
            uint8_t tag_a = VTAG_TAGGED, tag_b = VTAG_TAGGED;
            if (xir_ref_is_vreg(ins->args[0])) {
                tag_a = type_kind_to_vtag(xir_ref_ctype(ctx->func, ins->args[0]).kind);
            } else {
                tag_a = VTAG_I64;
            }  // const: not NUMERIC/TAGGED
            if (xir_ref_is_vreg(ins->args[1])) {
                tag_b = type_kind_to_vtag(xir_ref_ctype(ctx->func, ins->args[1]).kind);
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
                                 a64_ldr(SCRATCH_REG2, JIT_CTX_REG, XIR_JIT_LOAD_TAG_SCRATCH));
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
                    int32_t branch_off = (int32_t) (skip_target - patch_idx);
                    ctx->buf.code[patch_idx] = 0x54000000 | ((branch_off & 0x7FFFF) << 5);  // b.eq
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
                                 a64_ldr(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_LOAD_TAG_SCRATCH));
                    a64_buf_emit(&ctx->buf, a64_fmov_gp_to_fp(fb, gb));
                    // cmp tag, #4 (XR_TAG_F64)
                    a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, 4));
                    // b.eq .skip_scvtf (tag == F64 means float → fmov was correct)
                    uint32_t patch_idx = ctx->buf.count;
                    a64_buf_emit(&ctx->buf, a64_nop());
                    a64_buf_emit(&ctx->buf, a64_scvtf(fb, gb));
                    uint32_t skip_target = ctx->buf.count;
                    int32_t branch_off = (int32_t) (skip_target - patch_idx);
                    ctx->buf.code[patch_idx] = 0x54000000 | ((branch_off & 0x7FFFF) << 5);  // b.eq
                } else {
                    A64Reg gb = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                    fb = 31;  // scratch FP d31
                    a64_buf_emit(&ctx->buf, a64_scvtf(fb, gb));
                }
                // Emit float operation → rd (FP register)
                switch (ins->op) {
                    case XIR_RT_ADD:
                        a64_buf_emit(&ctx->buf, a64_fadd(rd, fa, fb));
                        break;
                    case XIR_RT_SUB:
                        a64_buf_emit(&ctx->buf, a64_fsub(rd, fa, fb));
                        break;
                    case XIR_RT_MUL:
                        a64_buf_emit(&ctx->buf, a64_fmul(rd, fa, fb));
                        break;
                    case XIR_RT_DIV:
                        a64_buf_emit(&ctx->buf, a64_fdiv(rd, fa, fb));
                        break;
                    case XIR_RT_MOD: {
                        // fmod: a - trunc(a/b) * b
                        a64_buf_emit(&ctx->buf, a64_fdiv(30, fa, fb));
                        a64_buf_emit(&ctx->buf, a64_fcvtzs(SCRATCH_REG, 30));
                        a64_buf_emit(&ctx->buf, a64_scvtf(30, SCRATCH_REG));
                        a64_buf_emit(&ctx->buf, a64_fmul(30, 30, fb));
                        a64_buf_emit(&ctx->buf, a64_fsub(rd, fa, 30));
                        break;
                    }
                    default:
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
        case XIR_RT_UNM: {
            uint8_t ta = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[0]);
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
        case XIR_RT_LT:
        case XIR_RT_LE:
        case XIR_RT_EQ: {
            uint8_t ta = XR_REP_I64, tb = XR_REP_I64;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XIR_REF_INDEX(ins->args[0]);
                if (ai < ctx->func->nvreg)
                    ta = ctx->func->vregs[ai].rep;
            }
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t bi = XIR_REF_INDEX(ins->args[1]);
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
                if (ins->op == XIR_RT_LT)
                    cc = A64_CC_LT;
                else if (ins->op == XIR_RT_LE)
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
        case XIR_LOAD: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            bool load_fp = (ins->rep == XR_REP_F64);
            if (load_fp)
                a64_buf_emit(&ctx->buf, a64_ldr_fp(rd, rn, 0));
            else
                a64_buf_emit(&ctx->buf, a64_ldr(rd, rn, 0));
            break;
        }
        case XIR_STORE: {
            A64Reg rn = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg rm = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            bool store_fp = false;
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg)
                    store_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }
            if (store_fp)
                a64_buf_emit(&ctx->buf, a64_str_fp(rm, rn, 0));
            else
                a64_buf_emit(&ctx->buf, a64_str(rm, rn, 0));
            break;
        }

        // LOAD32S: 32-bit sign-extending load from [base + const_offset]
        // Used for loading int32 fields like XrArray.length into 64-bit register
        case XIR_LOAD32S: {
            A64Reg base = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            // LDRSW Xt, [Xn, #offset] — sign-extend 32-bit to 64-bit
            // Encoding: 1011 1001 10 imm12 Rn:5 Rt:5 = 0xB9800000
            // imm12 = offset / 4 (scaled)
            XR_DCHECK(offset >= 0 && (offset % 4) == 0, "assertion failed");
            uint32_t imm12 = (uint32_t) (offset / 4);
            XR_DCHECK(imm12 < 4096, "assertion failed");
            uint32_t enc = 0xB9800000 | (imm12 << 10) | ((uint32_t) base << 5) | (uint32_t) rd;
            a64_buf_emit(&ctx->buf, enc);
            break;
        }

        // LOAD8Z: 8-bit zero-extending load from [addr]
        // Used for struct BOOL fields (1 byte native storage)
        case XIR_LOAD8Z: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // LDRB Wt, [Xn] — load byte, zero-extend to 64-bit
            // Encoding: 0011 1001 01 imm12 Rn:5 Rt:5 = 0x39400000
            a64_buf_emit(&ctx->buf, 0x39400000 | ((uint32_t) addr << 5) | (uint32_t) rd);
            break;
        }

        // LOAD8S: 8-bit sign-extending load from [addr] (for int8 fields)
        case XIR_LOAD8S: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // LDRSB Xt, [Xn] — sign-extend byte to 64-bit
            // Encoding: 0011 1001 10 imm12 Rn:5 Rt:5 = 0x39800000
            a64_buf_emit(&ctx->buf, 0x39800000 | ((uint32_t) addr << 5) | (uint32_t) rd);
            break;
        }

        // STORE8: 8-bit store [addr] = low byte of value
        case XIR_STORE8: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            // STRB Wt, [Xn] — store byte
            // Encoding: 0011 1001 00 imm12 Rn:5 Rt:5 = 0x39000000
            a64_buf_emit(&ctx->buf, 0x39000000 | ((uint32_t) addr << 5) | (uint32_t) val);
            break;
        }

        // LOAD16Z: 16-bit zero-extending load from [addr] (for uint16 fields)
        case XIR_LOAD16Z: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // LDRH Wt, [Xn] — load halfword, zero-extend to 64-bit
            // Encoding: 0111 1001 01 imm12 Rn:5 Rt:5 = 0x79400000
            a64_buf_emit(&ctx->buf, 0x79400000 | ((uint32_t) addr << 5) | (uint32_t) rd);
            break;
        }

        // LOAD16S: 16-bit sign-extending load from [addr] (for int16 fields)
        case XIR_LOAD16S: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // LDRSH Xt, [Xn] — sign-extend halfword to 64-bit
            // Encoding: 0111 1001 10 imm12 Rn:5 Rt:5 = 0x79800000
            a64_buf_emit(&ctx->buf, 0x79800000 | ((uint32_t) addr << 5) | (uint32_t) rd);
            break;
        }

        // STORE16: 16-bit store [addr] = low 16 bits of value
        case XIR_STORE16: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            // STRH Wt, [Xn] — store halfword
            // Encoding: 0111 1001 00 imm12 Rn:5 Rt:5 = 0x79000000
            a64_buf_emit(&ctx->buf, 0x79000000 | ((uint32_t) addr << 5) | (uint32_t) val);
            break;
        }

        // LOAD32Z: 32-bit zero-extending load from [addr] (for uint32 fields)
        case XIR_LOAD32Z: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // LDR Wt, [Xn] — load word, implicit zero-extend to 64-bit
            // Encoding: 1011 1001 01 imm12 Rn:5 Rt:5 = 0xB9400000
            a64_buf_emit(&ctx->buf, 0xB9400000 | ((uint32_t) addr << 5) | (uint32_t) rd);
            break;
        }

        // STORE32: 32-bit store [addr] = low 32 bits of value
        case XIR_STORE32: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            // STR Wt, [Xn] — store word
            // Encoding: 1011 1001 00 imm12 Rn:5 Rt:5 = 0xB9000000
            a64_buf_emit(&ctx->buf, 0xB9000000 | ((uint32_t) addr << 5) | (uint32_t) val);
            break;
        }

        // LOAD_F32: load 32-bit float from [addr], promote to f64
        // ARM64: LDR St, [Xn] + FCVT Dd, Ss (single → double)
        case XIR_LOAD_F32: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // LDR St, [Xn, #0] — 32-bit FP load
            // Encoding: 1011 1101 01 imm12 Rn:5 Rt:5 = 0xBD400000
            a64_buf_emit(&ctx->buf, 0xBD400000 | ((uint32_t) addr << 5) | (uint32_t) rd);
            // FCVT Dd, Sd — single-precision to double-precision
            // Encoding: 0001 1110 0010 0010 1100 00 Rn:5 Rd:5 = 0x1E22C000
            a64_buf_emit(&ctx->buf, 0x1E22C000 | ((uint32_t) rd << 5) | (uint32_t) rd);
            break;
        }

        // STORE_F32: truncate f64 to float, store 32-bit to [addr]
        // ARM64: FCVT Ss, Dd + STR Ss, [Xn]
        case XIR_STORE_F32: {
            A64Reg addr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            // FCVT S31, Dval — double to single into scratch FP reg 31
            // Encoding: 0001 1110 0110 0010 0100 00 Rn:5 Rd:5 = 0x1E624000
            a64_buf_emit(&ctx->buf, 0x1E624000 | ((uint32_t) val << 5) | 31u);
            // STR S31, [Xn, #0] — 32-bit FP store
            // Encoding: 1011 1101 00 imm12 Rn:5 Rt:5 = 0xBD000000
            a64_buf_emit(&ctx->buf, 0xBD000000 | ((uint32_t) addr << 5) | 31u);
            break;
        }

        // GUARD_BOUNDS: deopt if (unsigned)index >= (unsigned)length
        // args[0] = index (i64), args[1] = length (i64), dst = const(deopt_id)
        case XIR_GUARD_BOUNDS: {
            A64Reg idx_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg len_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            // CMP idx, len (unsigned comparison)
            a64_buf_emit(&ctx->buf, a64_cmp(idx_reg, len_reg));
            // Load deopt_id into x17
            {
                uint16_t did = 0xFFFF;
                if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                    uint32_t dci = XIR_REF_INDEX(ins->dst);
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
        case XIR_LOAD_FIELD: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            // When field type is unknown (Json dynamic fields), save the
            // runtime tag to jit_ctx scratch so rt.add can distinguish
            // int payloads from float bit patterns.
            // MUST happen BEFORE payload load: rd may alias obj register.
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t vi = XIR_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg && ins->ctype.kind == XIR_TK_UNKNOWN &&
                    ins->rep == XR_REP_I64) {
                    a64_buf_emit(&ctx->buf,
                                 a64_ldrb(SCRATCH_REG2, obj, offset + XIR_XRVALUE_TAG_OFFSET));
                    a64_buf_emit(&ctx->buf,
                                 a64_str(SCRATCH_REG2, JIT_CTX_REG, XIR_JIT_LOAD_TAG_SCRATCH));
                }
            }
            // Payload sits at byte 8 within the XrValue struct
            if (ins->rep == XR_REP_F64) {
                a64_buf_emit(&ctx->buf, a64_ldr_fp(rd, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET));
            } else {
                a64_buf_emit(&ctx->buf, a64_ldr(rd, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET));
            }
            break;
        }

        // STORE_FIELD: store complete XrValue (payload + tag) to object field
        // ins->rep  = explicit XrValue tag (0-15), or XIR_SF_TAG_RUNTIME
        // ins->dst   = const(byte_offset)
        // args[0]    = obj ptr
        // args[1]    = value to store
        case XIR_STORE_FIELD: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            A64Reg val = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            int32_t offset = 0;
            if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                uint32_t ci = XIR_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }

            // Determine if payload is F64 (needs FP store instruction)
            bool is_fp = false;
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg)
                    is_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }

            // Store payload at XrValue byte 8 (payload union)
            if (is_fp) {
                a64_buf_emit(&ctx->buf, a64_str_fp(val, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET));
            } else {
                a64_buf_emit(&ctx->buf, a64_str(val, obj, offset + XIR_XRVALUE_PAYLOAD_OFFSET));
            }

            // Store descriptor (tag + flags + heap_type) at XrValue byte 0-3.
            // Merged into a single 32-bit store to minimize instruction count.
            // Layout: [0]=tag, [1]=flags(0), [2-3]=heap_type
            uint8_t xr_tag = ins->rep;
            {
                bool is_ptr_val = false;
                uint32_t tag_val = 0;

                if (xr_tag == XIR_SF_TAG_RUNTIME) {
                    // Try vreg semantic tag first, then fall back to machine type.
                    tag_val = XR_TAG_PTR;
                    if (xir_ref_is_vreg(ins->args[1])) {
                        XirType vct = xir_ref_ctype(ctx->func, ins->args[1]);
                        uint8_t vk = type_kind_to_vtag(vct.kind);
                        if (vtag_is_concrete(vk)) {
                            tag_val = vtag_to_value_tag(vk);
                            is_ptr_val = xir_type_is_ptr(vct.kind);
                        } else {
                            // Fall back to machine type inference
                            uint32_t vi = XIR_REF_INDEX(ins->args[1]);
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
                    // SCRATCH_REG2 = gc_type (uint8 from GC header)
                    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, val, XIR_GC_HDR_TYPE_OFFSET));
                    // SCRATCH_REG = tag_val (bits [0..15])
                    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, (uint16_t) tag_val, 0));
                    // MOVK scratch, gc_type, LSL#16 → gc_type in bits [16..31]
                    // Cannot use movk with register; use add_lsl instead:
                    // scratch = tag_val + (gc_type << 16)
                    a64_buf_emit(&ctx->buf,
                                 a64_add_lsl(SCRATCH_REG, SCRATCH_REG, SCRATCH_REG2, 16));
                    a64_buf_emit(&ctx->buf,
                                 a64_str_w(SCRATCH_REG, obj, offset + XIR_XRVALUE_TAG_OFFSET));
                } else {
                    // Non-PTR: tag with heap_type=0, single 32-bit store
                    a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, (uint16_t) tag_val, 0));
                    a64_buf_emit(&ctx->buf,
                                 a64_str_w(SCRATCH_REG, obj, offset + XIR_XRVALUE_TAG_OFFSET));
                }
            }

            // Write barriers are managed by xir_insert_write_barriers pass
            // which inserts XIR_BARRIER_FWD after PTR STORE_FIELD ops.
            // Do NOT emit inline barriers here — their cbz skip offsets
            // conflict with the pass-inserted BARRIER_FWD instructions
            // that immediately follow this STORE_FIELD in the code stream.
            break;
        }

        // STORE_CORO: store value to JIT scratch at known byte offset
        // dst = const(byte_offset), args[0] = value to store
        // All STORE_CORO targets are JIT scratch fields (call_args, etc.)
        case XIR_STORE_CORO: {
            A64Reg val = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                uint32_t ci = XIR_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            // F64 vreg → FP register → use FP store instruction
            bool val_fp = false;
            if (xir_ref_is_vreg(ins->args[0])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[0]);
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
        case XIR_STORE_CORO_BYTE: {
            A64Reg val = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                uint32_t ci = XIR_REF_INDEX(ins->dst);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_strb(val, JIT_CTX_REG, offset));
            break;
        }

        // LOAD_CORO: load value from jit_ctx at known byte offset
        // args[0] = const(byte_offset), result = i64
        case XIR_LOAD_CORO: {
            int32_t offset = 0;
            if (!xir_ref_is_none(ins->args[0]) && xir_ref_is_const(ins->args[0])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_ldr(rd, JIT_CTX_REG, offset));
            xra_maybe_spill(ctx, ins->dst);
            break;
        }

        // LOAD_CORO_BYTE: load single byte from jit_ctx at known offset
        // args[0] = const(byte_offset), result = i64 (zero-extended)
        case XIR_LOAD_CORO_BYTE: {
            int32_t offset = 0;
            if (!xir_ref_is_none(ins->args[0]) && xir_ref_is_const(ins->args[0])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_ldrb(rd, JIT_CTX_REG, offset));
            xra_maybe_spill(ctx, ins->dst);
            break;
        }

        // TAG_LOAD: load tag field from XrValue in memory
        // args[0] = ptr to object, args[1] = const(byte_offset_to_tag)
        // Loads the 8-bit tag at the given byte offset
        // Builder computes: field_payload_offset + XIR_XRVALUE_TAG_OFFSET
        case XIR_TAG_LOAD: {
            A64Reg ptr = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            int32_t offset = 0;
            if (!xir_ref_is_none(ins->args[1]) && xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                offset = (int32_t) ctx->func->consts[ci].val.i64;
            }
            a64_buf_emit(&ctx->buf, a64_ldrb(rd, ptr, offset));
            break;
        }

        // TAG_CHECK: check tag == expected, deopt if mismatch
        // Same semantics as GUARD_TAG
        case XIR_TAG_CHECK: {
            A64Reg val_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG, val_reg, XIR_XRVALUE_TAG_OFFSET));
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
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
        // XrGCHeader.extra (uint16) at offset XIR_GC_EXTRA_OFFSET stores shape_id
        case XIR_GUARD_CLASS: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_ldrh(SCRATCH_REG, obj, XIR_GC_EXTRA_OFFSET));
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
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
        case XIR_GUARD_KLASS: {
            A64Reg obj = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // Load klass pointer from XrInstance (offset 16 after GCHeader)
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, obj, XIR_INSTANCE_KLASS_OFFSET));
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
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
        case XIR_CATCH: {
            a64_buf_emit(&ctx->buf, a64_ldr(rd, JIT_CTX_REG, XIR_JIT_EXCEPTION_OFFSET));
            // Clear exception = NULL
            a64_buf_emit(&ctx->buf, a64_str(A64_XZR, JIT_CTX_REG, XIR_JIT_EXCEPTION_OFFSET));
            break;
        }

        // THROW: handled by CALL_C to xr_jit_throw (builder emits CALL_C + GOTO/RET)
        // No separate codegen needed — XIR_THROW is never emitted directly

        // ALLOC: inline bump-pointer fast path + CALL_C slow path
        // args[0] = const(gc_type), args[1] = const(aligned_total_size)
        // Result: XrGCHeader* in rd
        //
        // Fast path (~18 instructions):
        //   1. Load gc = coro->coro_gc
        //   2. Bump check: cursor + size <= limit
        //   3. Commit: gc->cursor = new_cursor
        //   4. Init GC header inline (gc_next, type, marked, extra, objsize)
        //   5. Skip slow path
        // Slow path (~7 instructions):
        //   CALL_C to xr_jit_alloc(coro, type<<32|size)
        case XIR_ALLOC: {
            // args[0] = packed constant: (gc_extra << 8) | gc_type
            // args[1] = allocation size in bytes
            uint8_t gc_type = 0;
            uint16_t gc_extra = 0;
            uint32_t alloc_size = 0;
            if (xir_ref_is_const(ins->args[0])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
                int64_t packed = ctx->func->consts[ci].val.i64;
                gc_type = (uint8_t) (packed & 0xFF);
                gc_extra = (uint16_t) ((packed >> 8) & 0xFFFF);
            }
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                alloc_size = (uint32_t) ctx->func->consts[ci].val.i64;
            }
            alloc_size = (alloc_size + 7) & ~7u;

            // --- Fast path: inline bump-pointer ---
            // LDR x16, [x19, #XIR_CORO_GC_OFFSET]  — gc = coro->coro_gc
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, CORO_REG, XIR_CORO_GC_OFFSET));
            // CBZ x16, slow_path  (gc == NULL → slow path)
            uint32_t cbz_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_cbz(SCRATCH_REG, 0));  // patched below
            // LDR x17, [x16, #0]  — cursor = gc->immix.cursor
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, SCRATCH_REG, XIR_IMMIX_CURSOR_OFFSET));
            // ADD x17, x17, #size  — new_cursor = cursor + size
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG2, SCRATCH_REG2, alloc_size));
            // LDR rd, [x16, #8]  — limit (borrow rd temporarily)
            a64_buf_emit(&ctx->buf, a64_ldr(rd, SCRATCH_REG, XIR_IMMIX_LIMIT_OFFSET));
            // CMP x17, rd  — new_cursor <= limit?
            a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG2, rd));
            // B.HI slow_path
            uint32_t bhi_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched below

            // Commit: STR x17, [x16, #0]  — gc->cursor = new_cursor
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, SCRATCH_REG, XIR_IMMIX_CURSOR_OFFSET));
            // SUB rd, x17, #size  — rd = allocated GCHeader*
            a64_buf_emit(&ctx->buf, a64_sub_imm(rd, SCRATCH_REG2, alloc_size));

            // Init GC header inline:
            // STR xzr, [rd, #0]  — gc_next = NULL
            a64_buf_emit(&ctx->buf, a64_str(A64_XZR, rd, 0));
            // LDRB w17, [x16, #101]  — currentwhite
            a64_buf_emit(&ctx->buf,
                         a64_ldrb(SCRATCH_REG2, SCRATCH_REG, XIR_GC_CURRENTWHITE_OFFSET));
            // STRB w17, [rd, #9]  — marked = currentwhite
            a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, rd, XIR_GC_HDR_MARKED_OFFSET));
            // MOV w17, #gc_type
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, gc_type, 0));
            // STRB w17, [rd, #8]  — type
            a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, rd, XIR_GC_HDR_TYPE_OFFSET));
            // STRH extra, [rd, #10]  — extra (contains shape_id for Json objects)
            if (gc_extra != 0) {
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, gc_extra, 0));
                a64_buf_emit(&ctx->buf, a64_strh(SCRATCH_REG, rd, XIR_GC_HDR_EXTRA_OFFSET));
            } else {
                a64_buf_emit(&ctx->buf, a64_strh(A64_XZR, rd, XIR_GC_HDR_EXTRA_OFFSET));
            }
            // MOV w16, #alloc_size; STR w16, [rd, #12]  — objsize
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG, alloc_size & 0xFFFF, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG, rd, XIR_GC_HDR_OBJSIZE_OFFSET));

            // --- Inline alloc_post: GC bookkeeping without CALL_C stub ---
            // alloc_marks are DEFERRED: xr_immix_flush_marks() at slow path
            // entry marks all lines from mark_cursor to cursor in one batch.

            // 1. local_allgc linked list: block->local_allgc → obj → old_head
            //    block = rd & ~0x3FFF (16KB alignment)
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, ~(uint64_t) XIR_IMMIX_BLOCK_SIZE_MASK);
            a64_buf_emit(&ctx->buf, a64_and(SCRATCH_REG2, rd, SCRATCH_REG2));  // x17 = block
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG2,
                                            XIR_IMMIX_BLOCK_LOCAL_ALLGC_OFFSET));  // x16 = old_head
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, rd, 0));  // obj->gc_next = old_head
            a64_buf_emit(&ctx->buf,
                         a64_str(rd, SCRATCH_REG2,
                                 XIR_IMMIX_BLOCK_LOCAL_ALLGC_OFFSET));  // block->local_allgc = obj

            // 1b. block->alloc_count++, block->alloc_bytes += alloc_size
            a64_buf_emit(&ctx->buf,
                         a64_ldr_w(SCRATCH_REG, SCRATCH_REG2, XIR_IMMIX_BLOCK_ALLOC_COUNT_OFFSET));
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, 1));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG, SCRATCH_REG2, XIR_IMMIX_BLOCK_ALLOC_COUNT_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG, SCRATCH_REG2, XIR_IMMIX_BLOCK_ALLOC_BYTES_OFFSET));
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, alloc_size));
            a64_buf_emit(&ctx->buf,
                         a64_str(SCRATCH_REG, SCRATCH_REG2, XIR_IMMIX_BLOCK_ALLOC_BYTES_OFFSET));

            // 2. GC stats: totalbytes += size, GCdebt += size
            a64_buf_emit(&ctx->buf,
                         a64_ldr(SCRATCH_REG2, CORO_REG, XIR_CORO_GC_OFFSET));  // x17 = gc
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG2, XIR_GC_TOTALBYTES_OFFSET));
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, alloc_size));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, SCRATCH_REG2, XIR_GC_TOTALBYTES_OFFSET));
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG2, XIR_GC_GCDEBT_OFFSET));
            a64_buf_emit(&ctx->buf, a64_add_imm(SCRATCH_REG, SCRATCH_REG, alloc_size));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, SCRATCH_REG2, XIR_GC_GCDEBT_OFFSET));

            // B alloc_done (skip slow path)
            uint32_t b_done_idx = ctx->buf.count;
            a64_buf_emit(&ctx->buf, a64_nop());  // patched below

            // --- Slow path: CALL_C to xr_jit_alloc ---
            uint32_t slow_path_idx = ctx->buf.count;

            // Patch CBZ and B.HI to point here
            int32_t cbz_off = (int32_t) slow_path_idx - (int32_t) cbz_idx;
            ctx->buf.code[cbz_idx] = a64_cbz(SCRATCH_REG, cbz_off);
            int32_t bhi_off = (int32_t) slow_path_idx - (int32_t) bhi_idx;
            ctx->buf.code[bhi_idx] = a64_b_cond(A64_CC_HI, bhi_off);

            // Load func ptr and packed arg
            uint64_t fn_ptr = (uint64_t) (uintptr_t) &xr_jit_alloc;
            a64_load_imm64(&ctx->buf, SCRATCH_REG, fn_ptr);
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
                a64_buf_emit(&ctx->buf, a64_strh(SCRATCH_REG, rd, XIR_GC_HDR_EXTRA_OFFSET));
            }

            // Patch B to alloc_done
            uint32_t done_idx = ctx->buf.count;
            int32_t b_done_off = (int32_t) done_idx - (int32_t) b_done_idx;
            ctx->buf.code[b_done_idx] = a64_b(b_done_off);

            break;
        }

        case XIR_NOP:
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
        case XIR_SAFEPOINT: {
            record_safepoint(ctx);

            // Guard page poll: faults when page is armed (PROT_NONE)
            a64_buf_emit(&ctx->buf, a64_ldr_w(A64_XZR, SAFEPT_PAGE_REG, 0));

            ctx->has_safepoints = true;
            break;
        }

        // Write barriers: move args to scratch regs, BL to shared stub
        case XIR_BARRIER_FWD: {
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
        case XIR_BARRIER_BACK: {
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
        case XIR_GUARD_TAG: {
            A64Reg val_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // Load tag field: LDRB w16, [val_reg, #0] (XrValue.tag at byte 0)
            a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG, val_reg, XIR_XRVALUE_TAG_OFFSET));
            // Load expected tag
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                uint32_t expected = (uint32_t) ctx->func->consts[ci].val.raw;
                a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, expected & 0xFFF));
            } else {
                A64Reg exp_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
                a64_buf_emit(&ctx->buf, a64_cmp(SCRATCH_REG, exp_reg));
            }
            // Load deopt_id into x17 for the deopt stub
            {
                uint16_t did = 0xFFFF;
                if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                    uint32_t dci = XIR_REF_INDEX(ins->dst);
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
        case XIR_DEOPT: {
            // Load deopt_id into x17
            {
                uint16_t did = 0xFFFF;
                if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                    uint32_t dci = XIR_REF_INDEX(ins->dst);
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
        case XIR_GUARD_NONNULL: {
            A64Reg val_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // CBZ val_reg → deopt (but CBZ is forward branch, we use CMP+B.EQ)
            a64_buf_emit(&ctx->buf, a64_cmp_imm(val_reg, 0));
            // Load deopt_id into x17
            {
                uint16_t did = 0xFFFF;
                if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                    uint32_t dci = XIR_REF_INDEX(ins->dst);
                    did = (uint16_t) ctx->func->consts[dci].val.raw;
                }
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, did, 0));
            }
            add_patch(ctx, PATCH_DEOPT_EQ, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.EQ
            ctx->has_deopt = true;
            break;
        }

        // Guard shape: deopt if obj is null or shape_id != expected
        // args[0] = obj_ptr, args[1] = const(expected_shape_id)
        case XIR_GUARD_SHAPE: {
            A64Reg obj_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            // Load deopt_id into x17 (needed for both null and shape mismatch)
            uint16_t did = 0xFFFF;
            if (!xir_ref_is_none(ins->dst) && xir_ref_is_const(ins->dst)) {
                uint32_t dci = XIR_REF_INDEX(ins->dst);
                did = (uint16_t) ctx->func->consts[dci].val.raw;
            }
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, did, 0));
            // Null check: CBZ obj → deopt (prevents SIGSEGV on field access)
            add_patch(ctx, PATCH_DEOPT_CBZ, 0, obj_reg);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for CBZ
            // Alignment check: SSO string payloads are not 8-byte aligned.
            // If obj & 7 != 0, it's not a valid GC pointer → deopt.
            a64_buf_emit(&ctx->buf, a64_tst_imm(obj_reg, 0x7));
            add_patch(ctx, PATCH_DEOPT_NE, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.NE
            // Type check: deopt if gc.rep != XR_TJSON (prevents crash on non-Json objects)
            a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG, obj_reg, XIR_GC_HDR_TYPE_OFFSET));
            a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, 23));  // XR_TJSON = 23
            add_patch(ctx, PATCH_DEOPT_NE, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.NE
            // Load gc.extra (uint16_t at offset 10)
            a64_buf_emit(&ctx->buf, a64_ldrh(SCRATCH_REG, obj_reg, XIR_GC_HDR_EXTRA_OFFSET));
            // Extract shape_id: (extra & 0xFFFC) >> 2 = extra >> 2 (low bits are flags)
            a64_buf_emit(&ctx->buf, a64_lsr_imm(SCRATCH_REG, SCRATCH_REG, 2));
            // Compare with expected shape_id
            uint32_t ci = XIR_REF_INDEX(ins->args[1]);
            uint32_t expected_id = (uint32_t) ctx->func->consts[ci].val.raw;
            a64_buf_emit(&ctx->buf, a64_cmp_imm(SCRATCH_REG, expected_id));
            add_patch(ctx, PATCH_DEOPT_NE, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());  // placeholder for B.NE
            ctx->has_deopt = true;
            break;
        }

        // RT_ARRAY_NEW: create new array with given capacity via C helper
        // args[0] = capacity (const or vreg i64)
        // Result: ptr to XrArray in rd
        case XIR_RT_ARRAY_NEW: {
            // Load helper address to x16
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_rt_array_new);
            // Load capacity to x17
            if (xir_ref_is_const(ins->args[0])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
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
        case XIR_RT_ARRAY_PUSH: {
            // Store arr to call_args[0]
            A64Reg arr_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_str(arr_reg, JIT_CTX_REG, 0));
            // Store val to call_args[1]
            A64Reg val_reg = xra_arg(ctx, ins->args[1], SCRATCH_REG2);
            bool val_fp = false;
            if (xir_ref_is_vreg(ins->args[1])) {
                uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg)
                    val_fp = (ctx->func->vregs[vi].rep == XR_REP_F64);
            }
            if (val_fp)
                a64_buf_emit(&ctx->buf, a64_str_fp(val_reg, JIT_CTX_REG, 8));
            else
                a64_buf_emit(&ctx->buf, a64_str(val_reg, JIT_CTX_REG, 8));
            // Determine val value_tag for reconstruction.
            // If vreg has a known concrete vtag, convert to value_tag.
            // Otherwise 0xFF = UNKNOWN, patched at runtime from slot_runtime_tags.
            uint8_t val_tag = 0xFF;  // unknown
            if (xir_ref_is_vreg(ins->args[1])) {
                XirType vct = xir_ref_ctype(ctx->func, ins->args[1]);
                uint8_t vk = type_kind_to_vtag(vct.kind);
                if (vk != VTAG_TAGGED && vk != VTAG_NUMERIC) {
                    uint8_t vval = vtag_to_value_tag(vk);
                    if (vval != 0xFF)
                        val_tag = vval;
                }
            }
            // Write val_tag to call_arg_tags[1] (helper reads it via call_arg_tags)
            int32_t tag1_off = (int32_t) XIR_JIT_CALL_ARG_TAGS_OFFSET + 1;
            if (val_tag == 0xFF && xir_ref_is_vreg(ins->args[1])) {
                // Dynamic patch: load from slot_runtime_tags[bc_slot]
                uint32_t vi = XIR_REF_INDEX(ins->args[1]);
                if (vi < ctx->func->nvreg) {
                    int16_t bc_slot = ctx->func->vregs[vi].bc_slot;
                    if (bc_slot >= 0 && bc_slot < 256) {
                        int32_t src_off = (int32_t) XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + bc_slot;
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
            } else {
                a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, val_tag, 0));
                a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag1_off));
            }
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_rt_array_push);
            a64_load_imm64(&ctx->buf, SCRATCH_REG2, (uint64_t) val_tag);
            add_patch(ctx, PATCH_CALL_C, 0, A64_XZR);
            a64_buf_emit(&ctx->buf, a64_nop());
            ctx->has_call_c = true;
            break;
        }

        // RT_ARRAY_LEN: get array length via C helper
        // args[0] = arr (vreg PTR)
        // Result: i64 length in rd
        case XIR_RT_ARRAY_LEN: {
            A64Reg arr_reg = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_str(arr_reg, JIT_CTX_REG, 0));
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_rt_array_len);
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
        case XIR_RT_MAP_NEW: {
            a64_load_imm64(&ctx->buf, SCRATCH_REG, (uint64_t) (uintptr_t) xr_jit_rt_map_new);
            if (xir_ref_is_const(ins->args[0])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[0]);
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
        case XIR_RT_ISNULL: {
            // For PTR type: check if pointer is NULL
            A64Reg val = xra_arg(ctx, ins->args[0], SCRATCH_REG);
            a64_buf_emit(&ctx->buf, a64_cmp_imm(val, 0));
            // CSET rd, EQ (rd = 1 if val==0, else 0)
            a64_buf_emit(&ctx->buf, a64_cset(rd, A64_CC_EQ));
            break;
        }

        // RT_INDEX_GET / RT_INDEX_SET: delegate to existing CALL_C helpers
        // These shouldn't normally be emitted by the builder in JIT mode,
        // but handle gracefully by falling through to default with NOP.
        case XIR_RT_INDEX_GET:
        case XIR_RT_INDEX_SET:
        case XIR_RT_PRINT: {
            // These are handled via CALL_C in the builder, shouldn't reach here.
            // Emit NOP as safety fallback.
            fprintf(stderr, "[codegen] RT opcode %d should use CALL_C path\n", ins->op);
            a64_buf_emit(&ctx->buf, a64_nop());
            break;
        }

        // JIT CPS suspend sequence (AWAIT / CHAN_SEND / CHAN_RECV)
        // Saves all allocatable GP registers, calls await_block helper,
        // returns SUSPEND_MARKER if blocked, or inline-resumes with result.
        case XIR_SUSPEND: {
            // Get suspend_id from vreg metadata (stored by builder)
            uint32_t suspend_id = 0;
            if (xir_ref_is_vreg(ins->dst)) {
                uint32_t vi = XIR_REF_INDEX(ins->dst);
                if (vi < ctx->func->nvreg)
                    suspend_id = ctx->func->vregs[vi].call_arg_start;
            }

            // Get discard_result from args[1] (const)
            int64_t discard_result = 0;
            if (xir_ref_is_const(ins->args[1])) {
                uint32_t ci = XIR_REF_INDEX(ins->args[1]);
                discard_result = ctx->func->consts[ci].val.i64;
            }

            // 1. Record safepoint bitmap for GC
            uint32_t smap_id = record_safepoint(ctx);

            // 2. Load suspend_state pointer: x16 = coro->jit_suspend
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, CORO_REG, XIR_CORO_SUSPEND_PTR_OFFSET));

            // 3. Save x1-x15 to suspend_regs[0..14]
            a64_buf_emit(&ctx->buf, a64_stp(A64_X1, A64_X2, SCRATCH_REG, 0));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X3, A64_X4, SCRATCH_REG, 16));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X5, A64_X6, SCRATCH_REG, 32));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X7, A64_X8, SCRATCH_REG, 48));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X9, A64_X10, SCRATCH_REG, 64));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X11, A64_X12, SCRATCH_REG, 80));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X13, A64_X14, SCRATCH_REG, 96));
            a64_buf_emit(&ctx->buf, a64_str(A64_X15, SCRATCH_REG, 112));

            // 4. Save x20-x27 to suspend_regs[15..22]
            a64_buf_emit(&ctx->buf, a64_stp(A64_X20, A64_X21, SCRATCH_REG, 120));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X22, A64_X23, SCRATCH_REG, 136));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X24, A64_X25, SCRATCH_REG, 152));
            a64_buf_emit(&ctx->buf, a64_stp(A64_X26, A64_X27, SCRATCH_REG, 168));

            // 4b. Save spill slots to suspend_state.spill[0..nspill-1].
            // The resume entry creates a NEW stack frame whose spill area
            // is uninitialized; these values bridge the old and new frames.
            {
                uint32_t ns = ctx->xra ? ctx->xra->nspill : 0;
                if (ns > XIR_SUSPEND_SPILL_MAX)
                    ns = XIR_SUSPEND_SPILL_MAX;
                for (uint32_t s = 0; s < ns; s++) {
                    int32_t frame_off = SPILL_BASE + (int32_t) s * 8;
                    int32_t regs_off = XIR_SUSPEND_SPILL_OFF + (int32_t) s * 8;
                    // LDR x17, [SP, #frame_off]
                    a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG2, A64_SP, frame_off));
                    // STR x17, [x16, #regs_off]  (x16 = suspend_regs base)
                    a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG2, SCRATCH_REG, regs_off));
                }
            }

            // 5. Store suspend_id and smap_id
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) suspend_id, 0));
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, CORO_REG, XIR_CORO_SUSPEND_ID_OFFSET));
            a64_buf_emit(&ctx->buf, a64_movz(SCRATCH_REG2, (uint16_t) smap_id, 0));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, CORO_REG, XIR_CORO_SUSPEND_SMAP_OFFSET));
            // Update frame + jit_ctx smap for GC during blocked state
            a64_buf_emit(&ctx->buf, a64_str_w(SCRATCH_REG2, A64_FP, FRAME_SMAP_ID_OFFSET));
            a64_buf_emit(&ctx->buf,
                         a64_str_w(SCRATCH_REG2, JIT_CTX_REG, XIR_JIT_ACTIVE_SMAP_ID_OFFSET));

            // 6. Pre-store resume info BEFORE block helper (gopark pattern).
            // Once block_helper sets BLOCKED under lock, another worker may
            // wake and resume this coro immediately. resume_entry/proto must
            // already be valid at that point.
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, JIT_CTX_REG, XIR_JIT_CALL_PROTO_OFFSET));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, CORO_REG, XIR_CORO_RESUME_PROTO_OFFSET));
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, SCRATCH_REG, 376));
            a64_buf_emit(&ctx->buf, a64_str(SCRATCH_REG, CORO_REG, XIR_CORO_RESUME_ENTRY_OFFSET));

            // 7. Call block helper(coro, extra_arg)
            // Block helper selection: func metadata takes priority over default.
            // Channel ops store their helper in suspend_block_helpers[suspend_id].
            // AWAIT leaves it NULL → use default xr_jit_await_block.
            void *block_helper = ctx->func->suspend_block_helpers[suspend_id];
            int64_t helper_extra_arg = 0;
            if (!block_helper) {
                block_helper = (void *) xr_jit_await_block;
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
            a64_load_imm64(&ctx->buf, A64_X0, (uint64_t) XIR_SUSPEND_MARKER);
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
                int32_t off = (int32_t) here - (int32_t) cbnz_not_blocked;
                ctx->buf.code[cbnz_not_blocked] = a64_cbnz(A64_X0, off);
            }

            // Reload suspend pointer (x16 clobbered by BLR)
            a64_buf_emit(&ctx->buf, a64_ldr(SCRATCH_REG, CORO_REG, XIR_CORO_SUSPEND_PTR_OFFSET));

            // Reload x1-x15 from suspend_regs (x20-x27 survived as callee-saved)
            a64_buf_emit(&ctx->buf, a64_ldp(A64_X1, A64_X2, SCRATCH_REG, 0));
            a64_buf_emit(&ctx->buf, a64_ldp(A64_X3, A64_X4, SCRATCH_REG, 16));
            a64_buf_emit(&ctx->buf, a64_ldp(A64_X5, A64_X6, SCRATCH_REG, 32));
            a64_buf_emit(&ctx->buf, a64_ldp(A64_X7, A64_X8, SCRATCH_REG, 48));
            a64_buf_emit(&ctx->buf, a64_ldp(A64_X9, A64_X10, SCRATCH_REG, 64));
            a64_buf_emit(&ctx->buf, a64_ldp(A64_X11, A64_X12, SCRATCH_REG, 80));
            a64_buf_emit(&ctx->buf, a64_ldp(A64_X13, A64_X14, SCRATCH_REG, 96));
            a64_buf_emit(&ctx->buf, a64_ldr(A64_X15, SCRATCH_REG, 112));

            // Load await/channel result from suspend_state.result into dst register
            if (rd != A64_XZR) {
                a64_buf_emit(&ctx->buf, a64_ldr(rd, SCRATCH_REG, XIR_SUSPEND_RESULT_OFF));
            }

            // Load result_tag from suspend_state and write to runtime_tags[bc_slot].
            // This ensures downstream CALL_C helpers (e.g. xr_jit_rt_add) get the
            // correct type tag for the await/channel result after resume.
            {
                int16_t res_bc_slot = -1;
                if (xir_ref_is_vreg(ins->dst)) {
                    uint32_t vi = XIR_REF_INDEX(ins->dst);
                    if (vi < ctx->func->nvreg)
                        res_bc_slot = ctx->func->vregs[vi].bc_slot;
                }
                if (res_bc_slot >= 0 && res_bc_slot < 256) {
                    // LDRB w17, [x16, #result_tag_off]
                    a64_buf_emit(&ctx->buf, a64_ldrb(SCRATCH_REG2, SCRATCH_REG,
                                                     (int32_t) XIR_SUSPEND_RESULT_TAG_OFF));
                    // STRB w17, [x28, #runtime_tags + bc_slot]
                    int32_t tag_off = (int32_t) XIR_JIT_SLOT_RUNTIME_TAGS_OFFSET + res_bc_slot;
                    a64_buf_emit(&ctx->buf, a64_strb(SCRATCH_REG2, JIT_CTX_REG, tag_off));
                }
                // Record bc_slot for resume entry trampoline
                if (suspend_id < 16)
                    ctx->suspend_result_bc_slots[suspend_id] = res_bc_slot;
            }

            // Record continuation point for resume entry jump table
            if (suspend_id < 16) {
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
