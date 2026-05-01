/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_to_xir.c - Xi IR to XIR lowering for JIT compilation
 *
 * Directly translates Xi SSA values to XIR instructions.
 * Eliminates the Braun SSA reconstruction that the bytecode builder performs,
 * since Xi IR already has SSA form with precise types on every value.
 *
 * Coverage: arithmetic, comparison, bitwise, branches, phi nodes,
 * constants, type conversion, box/unbox.
 */

#include "xi_to_xir.h"
#include "xir.h"
#include "xir_ops.h"
#include "../ir/xi_rep.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xchunk.h"
#include <string.h>

/* ========== Lowering Context ========== */

typedef struct {
    XiFunc *xi_func;
    XirFunc *xir_func;
    XrProto *proto;
    XiSlotMap *slot_map;

    /* Xi block id → XirBlock* mapping */
    XirBlock **block_map;
    uint32_t block_map_size;

    /* Xi value id → XirRef mapping */
    XirRef *ref_map;
    uint32_t ref_map_size;

    bool error;
} LowerCtx;

/* ========== Helpers ========== */

/* Map Xi IR type to XIR machine representation */
static uint8_t xi_type_to_rep(struct XrType *type) {
    if (!type) return XR_REP_I64;
    switch (type->kind) {
        case XR_KIND_INT:   return XR_REP_I64;
        case XR_KIND_FLOAT: return XR_REP_F64;
        case XR_KIND_BOOL:  return XR_REP_I64;
        case XR_KIND_NULL:  return XR_REP_I64;
        case XR_KIND_VOID:  return XR_REP_I64;
        default:            return XR_REP_I64;  /* tagged as i64 in JIT */
    }
}

/* Check if a type is floating-point */
static bool is_float_type(struct XrType *type) {
    return type && type->kind == XR_KIND_FLOAT;
}

/* Get XirRef for a previously-lowered Xi value */
static XirRef get_ref(LowerCtx *ctx, XiValue *v) {
    XR_DCHECK(v != NULL, "get_ref: NULL value");
    XR_DCHECK(v->id < ctx->ref_map_size, "get_ref: value id out of range");
    return ctx->ref_map[v->id];
}

/* Store XirRef for an Xi value */
static void set_ref(LowerCtx *ctx, uint32_t value_id, XirRef ref) {
    XR_DCHECK(value_id < ctx->ref_map_size, "set_ref: value id out of range");
    ctx->ref_map[value_id] = ref;
}

/* Get XirBlock for an Xi block */
static XirBlock *get_block(LowerCtx *ctx, XiBlock *blk) {
    XR_DCHECK(blk != NULL, "get_block: NULL block");
    XR_DCHECK(blk->id < ctx->block_map_size, "get_block: block id out of range");
    return ctx->block_map[blk->id];
}

/* ========== Constant Lowering ========== */

static XirRef lower_const(LowerCtx *ctx, XiValue *v) {
    XR_DCHECK(v->op == XI_CONST, "lower_const: not a constant");
    struct XrType *type = v->type;
    if (!type) return xir_const_i64(ctx->xir_func, v->aux_int);

    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
            return xir_const_i64(ctx->xir_func, v->aux_int);
        case XR_KIND_FLOAT: {
            union { int64_t i; double d; } u;
            u.i = v->aux_int;
            return xir_const_f64(ctx->xir_func, u.d);
        }
        case XR_KIND_STRING:
            return xir_const_ptr(ctx->xir_func, v->aux);
        default:
            return xir_const_i64(ctx->xir_func, v->aux_int);
    }
}

/* ========== Arithmetic / Bitwise Lowering ========== */

static XirRef lower_binary_arith(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 2, "binary arith: expected 2 args");
    XirRef lhs = get_ref(ctx, v->args[0]);
    XirRef rhs = get_ref(ctx, v->args[1]);
    bool is_float = is_float_type(v->type);
    uint8_t rep = is_float ? XR_REP_F64 : XR_REP_I64;

    uint16_t xir_op;
    switch (v->op) {
        case XI_ADD:  xir_op = is_float ? XIR_FADD : XIR_ADD; break;
        case XI_SUB:  xir_op = is_float ? XIR_FSUB : XIR_SUB; break;
        case XI_MUL:  xir_op = is_float ? XIR_FMUL : XIR_MUL; break;
        case XI_DIV:  xir_op = is_float ? XIR_FDIV : XIR_DIV; break;
        case XI_MOD:  xir_op = XIR_MOD; break;
        case XI_BAND: xir_op = XIR_AND; break;
        case XI_BOR:  xir_op = XIR_OR;  break;
        case XI_BXOR: xir_op = XIR_XOR; break;
        case XI_SHL:  xir_op = XIR_SHL; break;
        case XI_SHR:  xir_op = XIR_SHR; break;
        default:
            ctx->error = true;
            return xir_const_i64(ctx->xir_func, 0);
    }
    return xir_emit(ctx->xir_func, blk, xir_op, rep, lhs, rhs);
}

static XirRef lower_unary(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "unary: expected 1 arg");
    XirRef arg = get_ref(ctx, v->args[0]);
    bool is_float = is_float_type(v->type);
    uint8_t rep = is_float ? XR_REP_F64 : XR_REP_I64;

    uint16_t xir_op;
    switch (v->op) {
        case XI_NEG:  xir_op = is_float ? XIR_FNEG : XIR_NEG; break;
        case XI_BNOT: xir_op = XIR_NOT; break;
        case XI_NOT:  xir_op = XIR_NOT; break;
        default:
            ctx->error = true;
            return xir_const_i64(ctx->xir_func, 0);
    }
    return xir_emit_unary(ctx->xir_func, blk, xir_op, rep, arg);
}

/* ========== Comparison Lowering ========== */

static XirRef lower_comparison(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 2, "comparison: expected 2 args");
    XirRef lhs = get_ref(ctx, v->args[0]);
    XirRef rhs = get_ref(ctx, v->args[1]);

    /* Determine if operands are float (check arg type, not result type) */
    bool is_float = is_float_type(v->args[0]->type);

    uint16_t xir_op;
    switch (v->op) {
        case XI_EQ: xir_op = is_float ? XIR_FEQ : XIR_EQ; break;
        case XI_NE: xir_op = is_float ? XIR_FNE : XIR_NE; break;
        case XI_LT: xir_op = is_float ? XIR_FLT : XIR_LT; break;
        case XI_LE: xir_op = is_float ? XIR_FLE : XIR_LE; break;
        case XI_GT: xir_op = is_float ? XIR_FLT : XIR_LT; break;
        case XI_GE: xir_op = is_float ? XIR_FLE : XIR_LE; break;
        default:
            ctx->error = true;
            return xir_const_i64(ctx->xir_func, 0);
    }

    /* GT/GE: swap operands (XIR only has LT/LE) */
    if (v->op == XI_GT || v->op == XI_GE) {
        XirRef tmp = lhs;
        lhs = rhs;
        rhs = tmp;
    }

    return xir_emit(ctx->xir_func, blk, xir_op, XR_REP_I64, lhs, rhs);
}

/* ========== Type Conversion Lowering ========== */

static XirRef lower_convert(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "convert: expected 1 arg");
    XirRef arg = get_ref(ctx, v->args[0]);

    bool src_float = is_float_type(v->args[0]->type);
    bool dst_float = is_float_type(v->type);

    if (!src_float && dst_float) {
        /* int → float */
        return xir_emit_unary(ctx->xir_func, blk, XIR_I2F, XR_REP_F64, arg);
    } else if (src_float && !dst_float) {
        /* float → int */
        return xir_emit_unary(ctx->xir_func, blk, XIR_F2I, XR_REP_I64, arg);
    }
    /* Same type — identity */
    return arg;
}

static XirRef lower_box(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "box: expected 1 arg");
    XirRef arg = get_ref(ctx, v->args[0]);
    struct XrType *src_type = v->args[0]->type;

    if (is_float_type(src_type)) {
        return xir_emit_unary(ctx->xir_func, blk, XIR_BOX_F64, XR_REP_I64, arg);
    }
    return xir_emit_unary(ctx->xir_func, blk, XIR_BOX_I64, XR_REP_I64, arg);
}

static XirRef lower_unbox(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    XR_DCHECK(v->nargs == 1, "unbox: expected 1 arg");
    XirRef arg = get_ref(ctx, v->args[0]);

    if (is_float_type(v->type)) {
        return xir_emit_unary(ctx->xir_func, blk, XIR_UNBOX_F64, XR_REP_F64, arg);
    }
    return xir_emit_unary(ctx->xir_func, blk, XIR_UNBOX_I64, XR_REP_I64, arg);
}

/* ========== Call / Closure / Print Lowering ========== */

/* Forward-declare JIT runtime bridges used by codegen */
struct XrCoroutine;
typedef struct { int64_t val; uint8_t tag; } XrJitResult;
extern XR_FUNC XrJitResult xr_jit_call_func(struct XrCoroutine *coro, int64_t nargs_encoded);
extern XR_FUNC XrJitResult xr_jit_closure_new(struct XrCoroutine *coro, int64_t proto_raw);

static XirRef lower_call(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    /* XI_CALL: args[0]=callee, args[1..n]=params */
    XR_DCHECK(v->nargs >= 1, "call: must have at least callee arg");
    uint16_t nargs = v->nargs - 1;

    /* Collect call args: [0]=closure/callee, [1..]=actual params */
    XirRef call_args[17];
    uint16_t total = (uint16_t)(1 + nargs);
    if (total > 16) {
        ctx->error = true;
        return xir_const_i64(ctx->xir_func, 0);
    }
    for (uint16_t i = 0; i < v->nargs; i++)
        call_args[i] = get_ref(ctx, v->args[i]);

    /* Generic call via xr_jit_call_func bridge */
    XirRef nargs_ref = xir_const_i64(ctx->xir_func, (int64_t)nargs);
    XirRef nargs_val = xir_emit_unary(ctx->xir_func, blk, XIR_CONST_I64,
                                       XR_REP_I64, nargs_ref);
    XirRef fn_ref = xir_const_ptr(ctx->xir_func, (void *)xr_jit_call_func);
    XirRef result = xir_emit(ctx->xir_func, blk, XIR_CALL_DIRECT,
                              XR_REP_I64, nargs_val, fn_ref);
    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
    xir_func_bind_call_args(ctx->xir_func, result, call_args, total);
    return result;
}

static XirRef lower_closure_new(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    /* XI_CLOSURE_NEW: aux=child XiFunc*, args=capture values */
    XirRef proto_ref = xir_const_ptr(ctx->xir_func, v->aux);
    XirRef fn_ref = xir_const_ptr(ctx->xir_func, (void *)xr_jit_closure_new);
    XirRef result = xir_emit(ctx->xir_func, blk, XIR_CALL_C,
                              XR_REP_I64, proto_ref, fn_ref);
    blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
    return result;
}

static XirRef lower_print(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    /* XI_PRINT: args[0..n]=values, aux_int=flags */
    for (uint16_t i = 0; i < v->nargs; i++) {
        XirRef arg = get_ref(ctx, v->args[i]);
        XirRef flags = xir_const_i64(ctx->xir_func, v->aux_int);
        xir_emit(ctx->xir_func, blk, XIR_RT_PRINT, XR_REP_I64, arg, flags);
        blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
    }
    return xir_const_i64(ctx->xir_func, 0);
}

/* ========== Value Lowering Dispatch ========== */

static XirRef lower_value(LowerCtx *ctx, XirBlock *blk, XiValue *v) {
    switch (v->op) {
        case XI_CONST:
            return lower_const(ctx, v);

        case XI_PARAM: {
            /* Parameters are pre-mapped during initialization */
            return get_ref(ctx, v);
        }

        /* Binary arithmetic + bitwise */
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_SHL: case XI_SHR:
            return lower_binary_arith(ctx, blk, v);

        /* Unary */
        case XI_NEG: case XI_BNOT: case XI_NOT:
            return lower_unary(ctx, blk, v);

        /* Comparison */
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
            return lower_comparison(ctx, blk, v);

        /* Type conversion */
        case XI_CONVERT:
            return lower_convert(ctx, blk, v);
        case XI_BOX:
            return lower_box(ctx, blk, v);
        case XI_UNBOX:
            return lower_unbox(ctx, blk, v);

        /* Null check */
        case XI_ISNULL: {
            XR_DCHECK(v->nargs == 1, "isnull: expected 1 arg");
            XirRef arg = get_ref(ctx, v->args[0]);
            XirRef zero = xir_const_i64(ctx->xir_func, 0);
            return xir_emit(ctx->xir_func, blk, XIR_EQ, XR_REP_I64, arg, zero);
        }

        /* Function call */
        case XI_CALL:
            return lower_call(ctx, blk, v);

        /* Closure creation */
        case XI_CLOSURE_NEW:
            return lower_closure_new(ctx, blk, v);

        /* Print */
        case XI_PRINT:
            return lower_print(ctx, blk, v);

        /* Shared (module-level) variables — lowered to LOAD/STORE via
         * the coro's shared_array pointer */
        case XI_GET_SHARED: {
            XirRef idx = xir_const_i64(ctx->xir_func, v->aux_int);
            return xir_emit_unary(ctx->xir_func, blk, XIR_LOAD, XR_REP_I64, idx);
        }
        case XI_SET_SHARED: {
            XR_DCHECK(v->nargs == 1, "set_shared: expected 1 arg");
            XirRef val = get_ref(ctx, v->args[0]);
            XirRef idx = xir_const_i64(ctx->xir_func, v->aux_int);
            xir_emit(ctx->xir_func, blk, XIR_STORE, XR_REP_I64, idx, val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            return val;
        }

        /* Upvalue access */
        case XI_LOAD_UPVAL: {
            XirRef idx = xir_const_i64(ctx->xir_func, v->aux_int);
            return xir_emit_unary(ctx->xir_func, blk, XIR_LOAD, XR_REP_I64, idx);
        }
        case XI_STORE_UPVAL: {
            XR_DCHECK(v->nargs == 1, "store_upval: expected 1 arg");
            XirRef val = get_ref(ctx, v->args[0]);
            XirRef idx = xir_const_i64(ctx->xir_func, v->aux_int);
            xir_emit(ctx->xir_func, blk, XIR_STORE, XR_REP_I64, idx, val);
            blk->ins[blk->nins - 1].flags |= XIR_FLAG_SIDE_EFFECT;
            return val;
        }

        /* Method call — same as generic call for now */
        case XI_CALL_METHOD:
        case XI_CALL_BUILTIN:
            return lower_call(ctx, blk, v);

        /* Extract multi-return result — placeholder (returns call ref) */
        case XI_EXTRACT: {
            XR_DCHECK(v->nargs == 1, "extract: expected 1 arg");
            return get_ref(ctx, v->args[0]);
        }

        default:
            /* Unsupported op — mark error, return dummy */
            ctx->error = true;
            return xir_const_i64(ctx->xir_func, 0);
    }
}

/* ========== Block Lowering ========== */

/* Lower all phi nodes in a block */
static void lower_phis(LowerCtx *ctx, XiBlock *xi_blk, XirBlock *xir_blk) {
    for (XiPhi *phi = xi_blk->phis; phi; phi = phi->next) {
        XiValue *pv = &phi->value;
        uint8_t rep = xi_type_to_rep(pv->type);
        XirPhi *xir_phi = xir_add_phi(ctx->xir_func, xir_blk, rep);
        XR_DCHECK(xir_phi != NULL, "lower_phis: xir_add_phi returned NULL");
        set_ref(ctx, pv->id, xir_phi->dst);
    }
}

/* Set phi arguments after all blocks are lowered (all refs resolved) */
static void resolve_phi_args(LowerCtx *ctx, XiBlock *xi_blk, XirBlock *xir_blk) {
    uint32_t pred_idx = 0;
    (void)pred_idx;

    for (XiPhi *phi = xi_blk->phis; phi; phi = phi->next) {
        XiValue *pv = &phi->value;
        XR_DCHECK(pv->nargs == xi_blk->npreds,
                  "phi arg count must match predecessor count");

        /* Find matching XirPhi by dst ref */
        XirRef phi_ref = get_ref(ctx, pv);
        XirPhi *xir_phi = xir_blk->phis;
        while (xir_phi && xir_phi->dst != phi_ref)
            xir_phi = xir_phi->next;
        XR_DCHECK(xir_phi != NULL, "resolve_phi_args: no matching XirPhi");

        for (uint16_t i = 0; i < pv->nargs; i++) {
            XirRef arg_ref = get_ref(ctx, pv->args[i]);
            xir_phi_set_arg(xir_phi, i, arg_ref);
        }
    }
}

/* Lower a single block's instructions */
static void lower_block_values(LowerCtx *ctx, XiBlock *xi_blk, XirBlock *xir_blk) {
    for (uint32_t i = 0; i < xi_blk->nvalues; i++) {
        XiValue *v = xi_blk->values[i];
        if (!v) continue;
        XirRef ref = lower_value(ctx, xir_blk, v);
        set_ref(ctx, v->id, ref);
    }
}

/* Set block terminator */
static void lower_terminator(LowerCtx *ctx, XiBlock *xi_blk, XirBlock *xir_blk) {
    switch (xi_blk->kind) {
        case XI_BLOCK_PLAIN:
            if (xi_blk->succs[0]) {
                XirBlock *target = get_block(ctx, xi_blk->succs[0]);
                xir_block_set_jmp(xir_blk, target);
            }
            break;

        case XI_BLOCK_IF: {
            XR_DCHECK(xi_blk->control != NULL, "IF block has no control");
            XR_DCHECK(xi_blk->succs[0] != NULL, "IF block has no then successor");
            XR_DCHECK(xi_blk->succs[1] != NULL, "IF block has no else successor");
            XirRef cond = get_ref(ctx, xi_blk->control);
            XirBlock *if_true = get_block(ctx, xi_blk->succs[0]);
            XirBlock *if_false = get_block(ctx, xi_blk->succs[1]);
            xir_block_set_br(xir_blk, cond, if_true, if_false);
            break;
        }

        case XI_BLOCK_RETURN: {
            XirRef ret_val;
            if (xi_blk->control) {
                ret_val = get_ref(ctx, xi_blk->control);
            } else {
                ret_val = xir_const_i64(ctx->xir_func, 0);
            }
            xir_block_set_ret(xir_blk, ret_val);
            break;
        }

        case XI_BLOCK_UNREACHABLE:
            /* No successors — leave as unreachable */
            break;

        default:
            ctx->error = true;
            break;
    }
}

/* ========== Main Entry Point ========== */

XR_FUNC struct XirFunc *xi_to_xir_lower(XiFunc *xi_func,
                                          struct XrProto *proto,
                                          XiSlotMap *slot_map,
                                          struct XrayIsolate *isolate) {
    XR_DCHECK(xi_func != NULL, "xi_to_xir_lower: NULL xi_func");
    (void)isolate;

    XirFunc *func = xir_func_new(xi_func->name);
    if (!func) return NULL;

    LowerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.xi_func = xi_func;
    ctx.xir_func = func;
    ctx.proto = proto;
    ctx.slot_map = slot_map;
    ctx.error = false;

    /* Allocate block map */
    ctx.block_map_size = xi_func->next_block_id;
    ctx.block_map = (XirBlock **)xr_calloc(ctx.block_map_size, sizeof(XirBlock *));
    if (!ctx.block_map) { xir_func_destroy(func); return NULL; }

    /* Allocate ref map */
    ctx.ref_map_size = xi_func->next_value_id;
    ctx.ref_map = (XirRef *)xr_calloc(ctx.ref_map_size, sizeof(XirRef));
    if (!ctx.ref_map) {
        xr_free(ctx.block_map);
        xir_func_destroy(func);
        return NULL;
    }

    /* Create all XirBlocks upfront (so forward jumps resolve) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XirBlock *xir_blk = xir_func_add_block(func, NULL);
        XR_DCHECK(xir_blk != NULL, "xi_to_xir_lower: block allocation failed");
        ctx.block_map[xi_blk->id] = xir_blk;
    }

    /* Set up predecessor edges */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XirBlock *xir_blk = get_block(&ctx, xi_blk);
        for (uint16_t p = 0; p < xi_blk->npreds; p++) {
            XirBlock *pred = get_block(&ctx, xi_blk->preds[p]);
            xir_block_add_pred(xir_blk, pred, &func->arena);
        }
    }

    /* Map parameters to XIR vregs */
    for (uint16_t i = 0; i < xi_func->nparams; i++) {
        XiValue *param = xi_func->params[i];
        if (!param) continue;
        uint8_t rep = xi_type_to_rep(param->type);
        XirRef vreg = xir_new_vreg(func, rep);
        set_ref(&ctx, param->id, vreg);
    }

    /* Lower phi nodes (create XirPhi with dst, defer arg resolution) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XirBlock *xir_blk = get_block(&ctx, xi_blk);
        lower_phis(&ctx, xi_blk, xir_blk);
    }

    /* Lower all block instructions */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XirBlock *xir_blk = get_block(&ctx, xi_blk);
        lower_block_values(&ctx, xi_blk, xir_blk);
    }

    /* Resolve phi arguments (now all refs are populated) */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XirBlock *xir_blk = get_block(&ctx, xi_blk);
        resolve_phi_args(&ctx, xi_blk, xir_blk);
    }

    /* Set block terminators */
    for (uint32_t i = 0; i < xi_func->nblocks; i++) {
        XiBlock *xi_blk = xi_func->blocks[i];
        if (!xi_blk) continue;
        XirBlock *xir_blk = get_block(&ctx, xi_blk);
        lower_terminator(&ctx, xi_blk, xir_blk);
    }

    /* Cleanup */
    xr_free(ctx.block_map);
    xr_free(ctx.ref_map);

    if (ctx.error) {
        xir_func_destroy(func);
        return NULL;
    }

    return func;
}
