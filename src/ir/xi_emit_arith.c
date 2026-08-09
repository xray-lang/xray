/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_emit_arith.c - Bytecode emission for arithmetic, comparison,
 *                   unary, bitwise, conversion, box/unbox, and type ops
 */

#include "xi_emit_internal.h"
#include "xi_emit_vm_gen.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"

/* Binary arithmetic / bitwise with instruction fusion for constant operands.
 * ADDI/SUBI/MULI use signed 16-bit immediate (int16_t, -32768..32767).
 * ADDK/SUBK/MULK/DIVK/MODK use a 16-bit constant pool index. */
XR_FUNC void xi_emit_arith(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    XR_DCHECK(v->nargs >= 2, "xi_emit_arith: need 2 args");
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    XiValue *lhs = v->args[0];
    XiValue *rhs = v->args[1];

    /* float32 arithmetic: emit dedicated single-precision opcodes with both
     * operands in registers (no immediate/const fusion). This keeps the VM
     * bit-identical with AOT, which narrows each f32 operand to native `float`
     * and rounds every op at single precision. */
    if (v->type && v->type->kind == XR_KIND_FLOAT && v->type->scalar_rep == XR_NATIVE_F32) {
        OpCode fop = v->op == XI_ADD   ? OP_ADD_F32
                     : v->op == XI_SUB ? OP_SUB_F32
                     : v->op == XI_MUL ? OP_MUL_F32
                     : v->op == XI_DIV ? OP_DIV_F32
                                       : OP_NOP;
        if (fop != OP_NOP) {
            XiEmitReg b = reg_of(ctx, lhs);
            XiEmitReg c = reg_of(ctx, rhs);
            if (ctx->status != XI_EMIT_OK)
                return;
            emit_inst(ctx, CREATE_ABC(fop, dst, b, c));
            return;
        }
    }

    /* Try fused immediate form: OP_ADDI/SUBI/MULI with signed 16-bit C */
    bool rhs_is_small_int = (rhs->op == XI_CONST && rhs->type && rhs->type->kind == XR_KIND_INT &&
                             rhs->aux_int >= -32768 && rhs->aux_int <= 32767);
    bool lhs_is_small_int = (lhs->op == XI_CONST && lhs->type && lhs->type->kind == XR_KIND_INT &&
                             lhs->aux_int >= -32768 && lhs->aux_int <= 32767);

    if (rhs_is_small_int && (v->op == XI_ADD || v->op == XI_SUB || v->op == XI_MUL)) {
        XiEmitReg b = reg_of(ctx, lhs);
        if (ctx->status != XI_EMIT_OK)
            return;
        int16_t imm = (int16_t) rhs->aux_int;
        OpCode fused = v->op == XI_ADD ? OP_ADDI : v->op == XI_SUB ? OP_SUBI : OP_MULI;
        emit_inst(ctx, CREATE_ABC(fused, dst, b, (uint16_t) imm));
        return;
    }

    /* Commutative immediate: try swapping if lhs is the constant */
    if (lhs_is_small_int && v->op == XI_ADD) {
        XiEmitReg b = reg_of(ctx, rhs);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_ADDI, dst, b, (uint16_t) (int16_t) lhs->aux_int));
        return;
    }
    if (lhs_is_small_int && v->op == XI_MUL) {
        XiEmitReg b = reg_of(ctx, rhs);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(OP_MULI, dst, b, (uint16_t) (int16_t) lhs->aux_int));
        return;
    }

    /* Try constant-pool form: ADDK/SUBK/MULK/DIVK/MODK for larger constants */
    bool rhs_is_const_num = (rhs->op == XI_CONST && rhs->type &&
                             (rhs->type->kind == XR_KIND_INT || rhs->type->kind == XR_KIND_FLOAT));
    bool lhs_is_const_num = (lhs->op == XI_CONST && lhs->type &&
                             (lhs->type->kind == XR_KIND_INT || lhs->type->kind == XR_KIND_FLOAT));
    if (rhs_is_const_num && !rhs_is_small_int && !xi_emit_divmod_uses_unsigned(v) &&
        (v->op == XI_ADD || v->op == XI_SUB || v->op == XI_MUL || v->op == XI_DIV ||
         v->op == XI_MOD)) {
        XiEmitReg b = reg_of(ctx, lhs);
        if (ctx->status != XI_EMIT_OK)
            return;
        int ki;
        if (rhs->type->kind == XR_KIND_INT) {
            ki = add_const_int(ctx, rhs->aux_int);
        } else {
            double fval;
            memcpy(&fval, &rhs->aux_int, sizeof(double));
            ki = add_const_float(ctx, fval);
        }
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t karg;
        if (!xi_emit_const_index_to_c(ctx, ki, &karg))
            return;
        OpCode kop = v->op == XI_ADD   ? OP_ADDK
                     : v->op == XI_SUB ? OP_SUBK
                     : v->op == XI_MUL ? OP_MULK
                     : v->op == XI_DIV ? OP_DIVK
                                       : OP_MODK;
        emit_inst(ctx, CREATE_ABC(kop, dst, b, karg));
        return;
    }
    /* Commutative constant-pool: swap lhs constant for ADD/MUL */
    if (lhs_is_const_num && !lhs_is_small_int && (v->op == XI_ADD || v->op == XI_MUL)) {
        XiEmitReg b = reg_of(ctx, rhs);
        if (ctx->status != XI_EMIT_OK)
            return;
        int ki;
        if (lhs->type->kind == XR_KIND_INT) {
            ki = add_const_int(ctx, lhs->aux_int);
        } else {
            double fval;
            memcpy(&fval, &lhs->aux_int, sizeof(double));
            ki = add_const_float(ctx, fval);
        }
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t karg;
        if (!xi_emit_const_index_to_c(ctx, ki, &karg))
            return;
        OpCode kop = v->op == XI_ADD ? OP_ADDK : OP_MULK;
        emit_inst(ctx, CREATE_ABC(kop, dst, b, karg));
        return;
    }

    /* Generic register-register form */
    XiEmitReg b = reg_of(ctx, lhs);
    XiEmitReg c = reg_of(ctx, rhs);
    if (ctx->status != XI_EMIT_OK)
        return;

    OpCode op = xi_emit_vm_template_opcode(v->op);
    if (xi_emit_compare_uses_unsigned(v)) {
        if (v->op == XI_LT || v->op == XI_GT)
            op = OP_CMP_LTU;
        else if (v->op == XI_LE || v->op == XI_GE)
            op = OP_CMP_LEU;
    }
    if (xi_emit_shr_uses_unsigned(v))
        op = OP_SHR_U;
    if (xi_emit_divmod_uses_unsigned(v))
        op = (v->op == XI_DIV) ? OP_DIV_U : OP_MOD_U;
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
        return;
    }
    emit_inst(ctx, CREATE_ABC(op, dst, b, c));
}

/* Unary negation */
XR_FUNC void xi_emit_neg(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    OpCode op = xi_emit_vm_template_opcode(v->op);
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(op, dst, src, 0));
}

/* Logical not */
XR_FUNC void xi_emit_not(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    OpCode op = xi_emit_vm_template_opcode(v->op);
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(op, dst, src, 0));
}

/* Bitwise not */
XR_FUNC void xi_emit_bnot(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    OpCode op = xi_emit_vm_template_opcode(v->op);
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(op, dst, src, 0));
}

XR_FUNC void xi_emit_exact_bit(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    bool rotate = v->op == XI_BIT_ROTL || v->op == XI_BIT_ROTR;
    bool binary = rotate || v->op == XI_BIT_MUL_HIGH;
    uint16_t expected = binary ? 2 : 1;
    if (v->nargs != expected || v->aux_int < 0 || v->aux_int > UINT8_MAX) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }

    OpCode op = v->op == XI_BIT_ROTL       ? OP_BIT_ROTL
                : v->op == XI_BIT_ROTR     ? OP_BIT_ROTR
                : v->op == XI_BIT_BSWAP    ? OP_BIT_BSWAP
                : v->op == XI_BIT_POPCOUNT ? OP_BIT_POPCOUNT
                : v->op == XI_BIT_CLZ      ? OP_BIT_CLZ
                : v->op == XI_BIT_CTZ      ? OP_BIT_CTZ
                : v->op == XI_BIT_MUL_HIGH ? OP_BIT_MUL_HIGH
                                           : OP_NOP;
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
        return;
    }

    if (!binary) {
        XiEmitReg src = reg_of(ctx, v->args[0]);
        if (ctx->status != XI_EMIT_OK)
            return;
        emit_inst(ctx, CREATE_ABC(op, dst, src, (uint8_t) v->aux_int));
        return;
    }

    if (ctx->next_reg + 2 > MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg base = (XiEmitReg) ctx->next_reg;
    ctx->next_reg += 2;
    if (ctx->next_reg > ctx->max_reg)
        ctx->max_reg = ctx->next_reg;
    for (uint16_t arg = 0; arg < 2; arg++) {
        XiEmitReg src = reg_of(ctx, v->args[arg]);
        if (ctx->status != XI_EMIT_OK)
            return;
        XiEmitReg target = (XiEmitReg) (base + arg);
        if (src != target)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, target, src, 0));
    }
    emit_inst(ctx, CREATE_ABC(op, dst, base, (uint8_t) v->aux_int));
}

/* Comparison ops -> CMP_* (produce bool in register) */
XR_FUNC void xi_emit_cmp(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg b = reg_of(ctx, v->args[0]);
    XiEmitReg c = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;

    OpCode op = xi_emit_vm_template_opcode(v->op);
    if (xi_emit_compare_uses_unsigned(v)) {
        if (v->op == XI_LT || v->op == XI_GT)
            op = OP_CMP_LTU;
        else if (v->op == XI_LE || v->op == XI_GE)
            op = OP_CMP_LEU;
    }
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
        return;
    }
    if (xi_emit_vm_template_swaps_args(v->op))
        emit_inst(ctx, CREATE_ABC(op, dst, c, b));
    else
        emit_inst(ctx, CREATE_ABC(op, dst, b, c));
}

/* Type conversion */
XR_FUNC void xi_emit_convert(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    struct XrType *target = v->type;
    if (!target) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    uint16_t conversion = xr_conversion_bytecode_pack(
        &v->conversion, (uint8_t) (ctx->target_data_layout->pointer.size * 8u));
    switch (target->kind) {
        case XR_KIND_INT:
            emit_inst(ctx, CREATE_ABC(OP_TOINT, dst, src, conversion));
            break;
        case XR_KIND_FLOAT:
            emit_inst(ctx, CREATE_ABC(OP_TOFLOAT, dst, src, conversion));
            break;
        case XR_KIND_STRING:
            emit_inst(ctx, CREATE_ABC(OP_TOSTRING, dst, src,
                                      xi_emit_tostring_hint_for_type(v->args[0]->type)));
            break;
        case XR_KIND_BOOL:
            emit_inst(ctx, CREATE_ABC(OP_TOBOOL, dst, src, 0));
            break;
        case XR_KIND_RUNE:
            emit_inst(ctx, CREATE_ABC(OP_TORUNE, dst, src, 0));
            break;
        case XR_KIND_POINTER:
            /* VM pointer values use the same address-width integer payload as
             * numeric addresses; the type distinction is compile-time only. */
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
            break;
        default:
            emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP);
            return;
    }
}

/* Box: wrap primitive into tagged value */
XR_FUNC void xi_emit_box(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    struct XrType *sty = v->args[0]->type;
    if (sty && sty->kind == XR_KIND_FLOAT)
        emit_inst(ctx, CREATE_ABC(OP_BOX_F64, dst, src, 0));
    else
        emit_inst(ctx, CREATE_ABC(OP_BOX_I64, dst, src, 0));
}

/* Unbox: extract primitive from tagged value */
XR_FUNC void xi_emit_unbox(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    struct XrType *dty = v->type;
    if (dty && dty->kind == XR_KIND_FLOAT)
        emit_inst(ctx, CREATE_ABC(OP_UNBOX_F64, dst, src, 0));
    else
        emit_inst(ctx, CREATE_ABC(OP_UNBOX_I64, dst, src, 0));
}

XR_FUNC void xi_emit_enum_descriptor_box(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!v || v->nargs < 1 || !v->args[0] || !v->enum_metadata_owner) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint32_t layout_id = v->enum_metadata_owner && v->enum_metadata_owner->kind == XR_KIND_ENUM
                             ? (v->enum_metadata_owner->enum_type.layout &&
                                        v->enum_metadata_owner->enum_type.layout->layout_id != 0
                                    ? v->enum_metadata_owner->enum_type.layout->layout_id
                                    : v->enum_metadata_owner->enum_type.layout_id)
                             : xr_type_enum_metadata_layout_id(v->type);
    uint8_t kind = v->enum_metadata_kind != 0 ? v->enum_metadata_kind
                                              : (uint8_t) xr_type_enum_metadata_kind(v->type);
    int64_t token = ((int64_t) layout_id << 8) | (int64_t) kind;
    xi_emit_i64_const_reg(ctx, dst, token);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ENUM_DESCRIPTOR_BOX, dst, src, dst));
}

XR_FUNC void xi_emit_enum_descriptor_unbox(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!v || v->nargs < 1 || !v->args[0]) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ENUM_DESCRIPTOR_UNBOX, dst, src, 0));
}

/* Narrow: truncate int64/double to sub-width, sign/zero-extend back.
 * Each XI_NARROW_* maps 1:1 to the corresponding OP_NARROW_*. */
XR_FUNC void xi_emit_narrow(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    OpCode op = xi_emit_vm_template_opcode(v->op);
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    emit_inst(ctx, CREATE_ABC(op, dst, src, 0));
}

/* Widen: sign/zero extend sub-width value in int64 register.
 * Each XI_WIDEN_* maps 1:1 to the corresponding OP_WIDEN_*. */
XR_FUNC void xi_emit_widen(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    OpCode op = xi_emit_vm_template_opcode(v->op);
    if (op == OP_NOP) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    emit_inst(ctx, CREATE_ABC(op, dst, src, 0));
}

/* Null check */
XR_FUNC void xi_emit_isnull(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_ISNULL_SET, dst, src, 0));
}

/* Type check: IS A B C — R[A] = (R[B] is R[C])
 * args[0] = value to check, args[1] = type value (int type-id or class) */
XR_FUNC void xi_emit_is(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 2) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    XiEmitReg type_reg = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (xr_type_is_enum_metadata((const XrType *) v->aux))
        emit_inst(ctx, CREATE_ABC(OP_IS_ENUM_DESCRIPTOR, dst, src, type_reg));
    else
        emit_inst(ctx, CREATE_ABC(OP_IS, dst, src, type_reg));
}

/* Type cast (as / as?) with runtime typeof check.
 * Encoding from lowerer:
 *   aux_int = (tid << 1) | is_safe   — tid is XrTypeId, -1 if unknown
 *   aux     = type name string (arena-allocated) for error messages */
XR_FUNC void xi_emit_as(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    bool is_safe = (v->aux_int & 1) != 0;
    int tid = v->aux_int >> 1;
    /* Unknown target type: degenerate to a move */
    if (tid < 0) {
        if (dst != src)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
        emit_inst(ctx, CREATE_ABC(OP_DUP, dst, 0, 0));
        return;
    }

    /* Primitive conversion casts (unsafe `as T` only).
     * Safe casts (`as T?`) must go through the typeof check below
     * so that type mismatch returns null instead of converting. */
    if (!is_safe) {
        if (tid == 8 /* XR_TID_INT */) {
            emit_inst(ctx, CREATE_ABC(OP_TOINT, dst, src, 0));
            return;
        }
        if (tid == 11 /* XR_TID_FLOAT */) {
            emit_inst(ctx, CREATE_ABC(OP_TOFLOAT, dst, src, 0));
            return;
        }
        if (tid == 12 /* XR_TID_STRING */) {
            emit_inst(ctx, CREATE_ABC(OP_TOSTRING, dst, src,
                                      xi_emit_tostring_hint_for_type(v->args[0]->type)));
            return;
        }
        if (tid == 1 /* XR_TID_BOOL */) {
            emit_inst(ctx, CREATE_ABC(OP_TOBOOL, dst, src, 0));
            return;
        }
        if (tid == XR_TID_RUNE) {
            emit_inst(ctx, CREATE_ABC(OP_TORUNE, dst, src, 0));
            return;
        }
    }

    if (dst != src)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));

    if (!is_safe) {
        int64_t mask = 0;
        if (tid >= 0 && tid < 63)
            mask |= (1LL << tid);

        int mask_k = add_const_int(ctx, mask);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t mask_arg = 0;
        if (!xi_emit_const_index_to_c(ctx, mask_k, &mask_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_CHECKTYPE, dst, mask_arg, 0));
        emit_inst(ctx, CREATE_ABC(OP_DUP, dst, 0, 0));
        return;
    }

    /* A safe cast asks the same question `is` does, so it runs the same test:
     * comparing typeof against the id would answer `int` for every integer and
     * could never accept a fixed-width target. */
    if (ctx->next_reg + 1 >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
    XiEmitReg tid_reg = (XiEmitReg) ctx->next_reg++;
    if (tid_reg >= ctx->max_reg)
        ctx->max_reg = (tid_reg + 1);

    int tid_k = add_const_int(ctx, tid);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t tid_arg = 0;
    if (!xi_emit_const_index_to_c(ctx, tid_k, &tid_arg))
        return;
    emit_inst(ctx, CREATE_ABx(OP_LOADK, tid_reg, tid_arg));
    emit_inst(ctx, CREATE_ABC(OP_IS, tmp, dst, tid_reg));
    emit_inst(ctx, CREATE_ABC(OP_TEST, tmp, 1, 0));
    int ok_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0)); /* placeholder */

    emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
    int end_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    int ok_target = current_pc(ctx);
    XrInstruction *ok_inst = PROTO_CODE_PTR(ctx->proto, ok_jmp_pc);
    *ok_inst = CREATE_sJ(OP_JMP, ok_target - (ok_jmp_pc + 1));
    XrInstruction *end_inst = PROTO_CODE_PTR(ctx->proto, end_jmp_pc);
    *end_inst = CREATE_sJ(OP_JMP, ok_target - (end_jmp_pc + 1));
    /* XI_AS borrows its input but produces an owned result. Retaining null is
     * a no-op, so one DUP at the control-flow join establishes the result's
     * independent owner for both successful and failed safe casts. */
    emit_inst(ctx, CREATE_ABC(OP_DUP, dst, 0, 0));
}

XR_FUNC void xi_emit_checktype(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;

    if (dst != src)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));

    int tid = (int) (v->aux_int >> 1);
    bool allow_null = (v->aux_int & 1) != 0;

    int64_t mask = 0;
    if (tid >= 0 && tid < 63)
        mask |= (1LL << tid);
    if (allow_null)
        mask |= (1LL << XR_TID_NULL);

    int mask_k = add_const_int(ctx, mask);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t mask_arg = 0;
    if (!xi_emit_const_index_to_c(ctx, mask_k, &mask_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_CHECKTYPE, dst, mask_arg, 0));
}

static void xi_emit_type_query(EmitCtx *ctx, XiValue *v, XiEmitReg dst, uint8_t op) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(op, dst, src, 0));
}

XR_FUNC void xi_emit_typeid(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    xi_emit_type_query(ctx, v, dst, OP_TYPEOF);
}

XR_FUNC void xi_emit_typename(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    xi_emit_type_query(ctx, v, dst, OP_TYPENAME);
}

XR_FUNC void xi_emit_len(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (!v || v->nargs != 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    emit_inst(ctx, CREATE_ABC(OP_LEN, dst, src, 0));
}
