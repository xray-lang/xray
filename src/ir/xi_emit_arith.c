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
    if (v->type && v->type->kind == XR_KIND_FLOAT && v->type->native_width == XR_NATIVE_F32) {
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
    if (rhs_is_const_num && !rhs_is_small_int &&
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
    switch (target->kind) {
        case XR_KIND_INT:
            emit_inst(ctx, CREATE_ABC(OP_TOINT, dst, src, 0));
            break;
        case XR_KIND_FLOAT:
            emit_inst(ctx, CREATE_ABC(OP_TOFLOAT, dst, src, 0));
            break;
        case XR_KIND_STRING:
            emit_inst(ctx, CREATE_ABC(OP_TOSTRING, dst, src, 0));
            break;
        case XR_KIND_BOOL:
            emit_inst(ctx, CREATE_ABC(OP_TOBOOL, dst, src, 0));
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
    const char *tname = v->aux ? (const char *) v->aux : "unknown";

    /* Unknown target type: degenerate to a move */
    if (tid < 0) {
        if (dst != src)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
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
            emit_inst(ctx, CREATE_ABC(OP_TOSTRING, dst, src, 0));
            return;
        }
        if (tid == 1 /* XR_TID_BOOL */) {
            emit_inst(ctx, CREATE_ABC(OP_TOBOOL, dst, src, 0));
            return;
        }
    }

    if (dst != src)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));

    /* OP_TYPEOF tmp, dst, 0 */
    if (ctx->next_reg >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
    if (tmp >= ctx->max_reg)
        ctx->max_reg = (tmp + 1);
    emit_inst(ctx, CREATE_ABC(OP_TYPEOF, tmp, dst, 0));

    int tid_k = add_const_int(ctx, tid);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t tid_arg = 0;
    if (!xi_emit_const_index_to_c(ctx, tid_k, &tid_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_EQK, tmp, tid_arg, 1));
    int ok_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0)); /* placeholder */

    if (is_safe) {
        emit_inst(ctx, CREATE_ABC(OP_LOADNULL, dst, 0, 0));
        int end_jmp_pc = current_pc(ctx);
        emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
        int ok_target = current_pc(ctx);
        XrInstruction *ok_inst = PROTO_CODE_PTR(ctx->proto, ok_jmp_pc);
        *ok_inst = CREATE_sJ(OP_JMP, ok_target - (ok_jmp_pc + 1));
        XrInstruction *end_inst = PROTO_CODE_PTR(ctx->proto, end_jmp_pc);
        *end_inst = CREATE_sJ(OP_JMP, ok_target - (end_jmp_pc + 1));
    } else {
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf), "Type cast failed: expected %s", tname);
        int err_k = add_const_string(ctx, err_buf);
        if (ctx->status != XI_EMIT_OK)
            return;
        if (ctx->next_reg >= MAX_REGS) {
            emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
            return;
        }
        XiEmitReg err_reg = (XiEmitReg) ctx->next_reg++;
        if (err_reg >= ctx->max_reg)
            ctx->max_reg = (err_reg + 1);
        emit_inst(ctx, CREATE_ABx(OP_LOADK, err_reg, err_k));
        emit_inst(ctx, CREATE_ABC(OP_THROW, err_reg, 0, 0));
        int ok_target = current_pc(ctx);
        XrInstruction *ok_inst = PROTO_CODE_PTR(ctx->proto, ok_jmp_pc);
        *ok_inst = CREATE_sJ(OP_JMP, ok_target - (ok_jmp_pc + 1));
    }
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

    if (ctx->next_reg >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg tmp = (XiEmitReg) ctx->next_reg++;
    if (tmp >= ctx->max_reg)
        ctx->max_reg = (tmp + 1);
    emit_inst(ctx, CREATE_ABC(OP_TYPEOF, tmp, dst, 0));

    int tid = (int) (v->aux_int >> 1);
    bool allow_null = (v->aux_int & 1) != 0;
    const char *tname = v->aux ? (const char *) v->aux : "unknown";

    int tid_k = add_const_int(ctx, tid);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint16_t tid_arg = 0;
    if (!xi_emit_const_index_to_c(ctx, tid_k, &tid_arg))
        return;
    emit_inst(ctx, CREATE_ABC(OP_EQK, tmp, tid_arg, 1));
    int ok_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));

    int null_ok_jmp_pc = -1;
    if (allow_null && tid != 0) {
        int null_k = add_const_int(ctx, 0);
        if (ctx->status != XI_EMIT_OK)
            return;
        uint16_t null_arg = 0;
        if (!xi_emit_const_index_to_c(ctx, null_k, &null_arg))
            return;
        emit_inst(ctx, CREATE_ABC(OP_EQK, tmp, null_arg, 1));
        null_ok_jmp_pc = current_pc(ctx);
        emit_inst(ctx, CREATE_sJ(OP_JMP, 0));
    }

    char err_buf[128];
    snprintf(err_buf, sizeof(err_buf), "Type check failed: expected %s", tname);
    int err_k = add_const_string(ctx, err_buf);
    if (ctx->status != XI_EMIT_OK)
        return;
    if (ctx->next_reg >= MAX_REGS) {
        emit_error(ctx, XI_EMIT_ERR_TOO_MANY_REGS);
        return;
    }
    XiEmitReg err_reg = (XiEmitReg) ctx->next_reg++;
    if (err_reg >= ctx->max_reg)
        ctx->max_reg = (err_reg + 1);
    emit_inst(ctx, CREATE_ABx(OP_LOADK, err_reg, err_k));
    emit_inst(ctx, CREATE_ABC(OP_THROW, err_reg, 0, 0));

    int ok_target = current_pc(ctx);
    XrInstruction *ok_inst = PROTO_CODE_PTR(ctx->proto, ok_jmp_pc);
    *ok_inst = CREATE_sJ(OP_JMP, ok_target - (ok_jmp_pc + 1));
    if (null_ok_jmp_pc >= 0) {
        XrInstruction *null_ok_inst = PROTO_CODE_PTR(ctx->proto, null_ok_jmp_pc);
        *null_ok_inst = CREATE_sJ(OP_JMP, ok_target - (null_ok_jmp_pc + 1));
    }
}

/* typeof(x) / typename(x) */
XR_FUNC void xi_emit_typeof(EmitCtx *ctx, XiValue *v, XiEmitReg dst) {
    if (v->nargs < 1) {
        emit_error(ctx, XI_EMIT_ERR_INTERNAL);
        return;
    }
    XiEmitReg src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK)
        return;
    uint8_t tyop = v->aux_int == 1 ? OP_TYPENAME : OP_TYPEOF;
    emit_inst(ctx, CREATE_ABC(tyop, dst, src, 0));
}
