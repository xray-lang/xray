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
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_names.h"

/* Binary arithmetic / bitwise with instruction fusion for constant operands.
 * ADDI/SUBI/MULI use signed 8-bit immediate (int8_t, -128..127).
 * ADDK/SUBK/MULK/DIVK use constant pool index. */
XR_FUNC void xi_emit_arith(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    XR_DCHECK(v->nargs >= 2, "xi_emit_arith: need 2 args");
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }

    XiValue *lhs = v->args[0];
    XiValue *rhs = v->args[1];

    /* Try fused immediate form: OP_ADDI/SUBI/MULI with signed 8-bit C */
    bool rhs_is_small_int = (rhs->op == XI_CONST && rhs->type &&
                             rhs->type->kind == XR_KIND_INT &&
                             rhs->aux_int >= -128 && rhs->aux_int <= 127);
    bool lhs_is_small_int = (lhs->op == XI_CONST && lhs->type &&
                             lhs->type->kind == XR_KIND_INT &&
                             lhs->aux_int >= -128 && lhs->aux_int <= 127);

    if (rhs_is_small_int &&
        (v->op == XI_ADD || v->op == XI_SUB || v->op == XI_MUL)) {
        uint8_t b = reg_of(ctx, lhs);
        if (ctx->status != XI_EMIT_OK) return;
        int8_t imm = (int8_t)rhs->aux_int;
        OpCode fused = v->op == XI_ADD ? OP_ADDI :
                       v->op == XI_SUB ? OP_SUBI : OP_MULI;
        emit_inst(ctx, CREATE_ABC(fused, dst, b, (uint8_t)imm));
        return;
    }

    /* Commutative immediate: try swapping if lhs is the constant */
    if (lhs_is_small_int && v->op == XI_ADD) {
        uint8_t b = reg_of(ctx, rhs);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_ADDI, dst, b, (uint8_t)(int8_t)lhs->aux_int));
        return;
    }
    if (lhs_is_small_int && v->op == XI_MUL) {
        uint8_t b = reg_of(ctx, rhs);
        if (ctx->status != XI_EMIT_OK) return;
        emit_inst(ctx, CREATE_ABC(OP_MULI, dst, b, (uint8_t)(int8_t)lhs->aux_int));
        return;
    }

    /* Try constant-pool form: ADDK/SUBK/MULK/DIVK/MODK for larger constants */
    bool rhs_is_const_num = (rhs->op == XI_CONST && rhs->type &&
        (rhs->type->kind == XR_KIND_INT || rhs->type->kind == XR_KIND_FLOAT));
    bool lhs_is_const_num = (lhs->op == XI_CONST && lhs->type &&
        (lhs->type->kind == XR_KIND_INT || lhs->type->kind == XR_KIND_FLOAT));
    if (rhs_is_const_num && !rhs_is_small_int &&
        (v->op == XI_ADD || v->op == XI_SUB ||
         v->op == XI_MUL || v->op == XI_DIV || v->op == XI_MOD)) {
        uint8_t b = reg_of(ctx, lhs);
        if (ctx->status != XI_EMIT_OK) return;
        int ki;
        if (rhs->type->kind == XR_KIND_INT) {
            ki = add_const_int(ctx, rhs->aux_int);
        } else {
            double fval;
            memcpy(&fval, &rhs->aux_int, sizeof(double));
            ki = add_const_float(ctx, fval);
        }
        if (ctx->status != XI_EMIT_OK) return;
        OpCode kop = v->op == XI_ADD ? OP_ADDK :
                     v->op == XI_SUB ? OP_SUBK :
                     v->op == XI_MUL ? OP_MULK :
                     v->op == XI_DIV ? OP_DIVK : OP_MODK;
        emit_inst(ctx, CREATE_ABC(kop, dst, b, (uint8_t)ki));
        return;
    }
    /* Commutative constant-pool: swap lhs constant for ADD/MUL */
    if (lhs_is_const_num && !lhs_is_small_int &&
        (v->op == XI_ADD || v->op == XI_MUL)) {
        uint8_t b = reg_of(ctx, rhs);
        if (ctx->status != XI_EMIT_OK) return;
        int ki;
        if (lhs->type->kind == XR_KIND_INT) {
            ki = add_const_int(ctx, lhs->aux_int);
        } else {
            double fval;
            memcpy(&fval, &lhs->aux_int, sizeof(double));
            ki = add_const_float(ctx, fval);
        }
        if (ctx->status != XI_EMIT_OK) return;
        OpCode kop = v->op == XI_ADD ? OP_ADDK : OP_MULK;
        emit_inst(ctx, CREATE_ABC(kop, dst, b, (uint8_t)ki));
        return;
    }

    /* Generic register-register form */
    uint8_t b = reg_of(ctx, lhs);
    uint8_t c = reg_of(ctx, rhs);
    if (ctx->status != XI_EMIT_OK) return;

    OpCode op;
    switch (v->op) {
        case XI_ADD:  op = OP_ADD;  break;
        case XI_SUB:  op = OP_SUB;  break;
        case XI_MUL:  op = OP_MUL;  break;
        case XI_DIV:  op = OP_DIV;  break;
        case XI_MOD:  op = OP_MOD;  break;
        case XI_BAND: op = OP_BAND; break;
        case XI_BOR:  op = OP_BOR;  break;
        case XI_BXOR: op = OP_BXOR; break;
        case XI_SHL:  op = OP_SHL;  break;
        case XI_SHR:  op = OP_SHR;  break;
        default: op = OP_NOP; break;
    }
    emit_inst(ctx, CREATE_ABC(op, dst, b, c));
}

/* Unary negation */
XR_FUNC void xi_emit_neg(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    emit_inst(ctx, CREATE_ABC(OP_UNM, dst, reg_of(ctx, v->args[0]), 0));
}

/* Logical not */
XR_FUNC void xi_emit_not(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    emit_inst(ctx, CREATE_ABC(OP_NOT, dst, reg_of(ctx, v->args[0]), 0));
}

/* Bitwise not */
XR_FUNC void xi_emit_bnot(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    emit_inst(ctx, CREATE_ABC(OP_BNOT, dst, reg_of(ctx, v->args[0]), 0));
}

/* Comparison ops -> CMP_* (produce bool in register) */
XR_FUNC void xi_emit_cmp(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t b = reg_of(ctx, v->args[0]);
    uint8_t c = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;

    switch (v->op) {
        case XI_EQ: emit_inst(ctx, CREATE_ABC(OP_CMP_EQ, dst, b, c)); break;
        case XI_NE: emit_inst(ctx, CREATE_ABC(OP_CMP_NE, dst, b, c)); break;
        case XI_LT: emit_inst(ctx, CREATE_ABC(OP_CMP_LT, dst, b, c)); break;
        case XI_LE: emit_inst(ctx, CREATE_ABC(OP_CMP_LE, dst, b, c)); break;
        /* GT/GE: swap args */
        case XI_GT: emit_inst(ctx, CREATE_ABC(OP_CMP_LT, dst, c, b)); break;
        case XI_GE: emit_inst(ctx, CREATE_ABC(OP_CMP_LE, dst, c, b)); break;
        /* Strict (identity) comparison */
        case XI_EQ_STRICT: emit_inst(ctx, CREATE_ABC(OP_CMP_EQ_STRICT, dst, b, c)); break;
        case XI_NE_STRICT: emit_inst(ctx, CREATE_ABC(OP_CMP_NE_STRICT, dst, b, c)); break;
        default: break;
    }
}

/* Type conversion */
XR_FUNC void xi_emit_convert(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    struct XrType *target = v->type;
    if (!target) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    switch (target->kind) {
        case XR_KIND_INT:   emit_inst(ctx, CREATE_ABC(OP_TOINT, dst, src, 0)); break;
        case XR_KIND_FLOAT: emit_inst(ctx, CREATE_ABC(OP_TOFLOAT, dst, src, 0)); break;
        case XR_KIND_STRING:emit_inst(ctx, CREATE_ABC(OP_TOSTRING, dst, src, 0)); break;
        case XR_KIND_BOOL:  emit_inst(ctx, CREATE_ABC(OP_TOBOOL, dst, src, 0)); break;
        default: emit_error(ctx, XI_EMIT_ERR_UNSUPPORTED_OP); return;
    }
}

/* Box: wrap primitive into tagged value */
XR_FUNC void xi_emit_box(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    struct XrType *sty = v->args[0]->type;
    if (sty && sty->kind == XR_KIND_FLOAT)
        emit_inst(ctx, CREATE_ABC(OP_BOX_F64, dst, src, 0));
    else
        emit_inst(ctx, CREATE_ABC(OP_BOX_I64, dst, src, 0));
}

/* Unbox: extract primitive from tagged value */
XR_FUNC void xi_emit_unbox(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    struct XrType *dty = v->type;
    if (dty && dty->kind == XR_KIND_FLOAT)
        emit_inst(ctx, CREATE_ABC(OP_UNBOX_F64, dst, src, 0));
    else
        emit_inst(ctx, CREATE_ABC(OP_UNBOX_I64, dst, src, 0));
}

/* Null check */
XR_FUNC void xi_emit_isnull(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_ISNULL_SET, dst, src, 0));
}

/* Type check: IS A B C — R[A] = (R[B] is R[C])
 * args[0] = value to check, args[1] = type value (int type-id or class) */
XR_FUNC void xi_emit_is(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 2) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    uint8_t type_reg = reg_of(ctx, v->args[1]);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_IS, dst, src, type_reg));
}

/* Type cast (as / as?) with runtime typeof check */
XR_FUNC void xi_emit_as(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    bool is_safe = (v->aux_int == 1);
    struct XrType *target = (struct XrType *)v->aux;

    /* Map compile-time XrType kind to runtime XrTypeId */
    int tid = -1;
    if (target) {
        switch (target->kind) {
            case XR_KIND_INT:    tid = XR_TID_INT;    break;
            case XR_KIND_FLOAT:  tid = XR_TID_FLOAT;  break;
            case XR_KIND_STRING: tid = XR_TID_STRING; break;
            case XR_KIND_BOOL:   tid = XR_TID_BOOL;   break;
            case XR_KIND_JSON:   tid = XR_TID_JSON;   break;
            case XR_KIND_ARRAY:  tid = XR_TID_ARRAY;  break;
            default:             tid = -1;            break;
        }
    }

    /* Unknown target type: degenerate to a move */
    if (tid < 0) {
        if (dst != src)
            emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));
        return;
    }

    if (dst != src)
        emit_inst(ctx, CREATE_ABC(OP_MOVE, dst, src, 0));

    /* OP_TYPEOF tmp, dst, 0 */
    uint8_t tmp = ctx->next_reg++;
    if (tmp >= ctx->max_reg) ctx->max_reg = (uint8_t)(tmp + 1);
    emit_inst(ctx, CREATE_ABC(OP_TYPEOF, tmp, dst, 0));

    int tid_k = add_const_int(ctx, tid);
    if (ctx->status != XI_EMIT_OK) return;
    emit_inst(ctx, CREATE_ABC(OP_EQK, tmp, (uint8_t)tid_k, 1));
    int ok_jmp_pc = current_pc(ctx);
    emit_inst(ctx, CREATE_sJ(OP_JMP, 0));  /* placeholder */

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
        const char *tname = target ? xr_type_to_string(target) : "unknown";
        char err_buf[128];
        snprintf(err_buf, sizeof(err_buf),
                 "Type cast failed: expected %s", tname);
        int err_k = add_const_string(ctx, err_buf);
        if (ctx->status != XI_EMIT_OK) return;
        uint8_t err_reg = ctx->next_reg++;
        if (err_reg >= ctx->max_reg) ctx->max_reg = (uint8_t)(err_reg + 1);
        emit_inst(ctx, CREATE_ABx(OP_LOADK, err_reg, err_k));
        emit_inst(ctx, CREATE_ABC(OP_THROW, err_reg, 0, 0));
        int ok_target = current_pc(ctx);
        XrInstruction *ok_inst = PROTO_CODE_PTR(ctx->proto, ok_jmp_pc);
        *ok_inst = CREATE_sJ(OP_JMP, ok_target - (ok_jmp_pc + 1));
    }
}

/* typeof(x) / typename(x) */
XR_FUNC void xi_emit_typeof(EmitCtx *ctx, XiValue *v, uint8_t dst) {
    if (v->nargs < 1) { emit_error(ctx, XI_EMIT_ERR_INTERNAL); return; }
    uint8_t src = reg_of(ctx, v->args[0]);
    if (ctx->status != XI_EMIT_OK) return;
    uint8_t tyop = v->aux_int == 1 ? OP_TYPENAME : OP_TYPEOF;
    emit_inst(ctx, CREATE_ABC(tyop, dst, src, 0));
}
