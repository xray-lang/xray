/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_opt.c - SSA optimization passes for Xi IR
 *
 * Passes operate on XiFunc in-place, preserving SSA invariants.
 * All passes are safe for functions produced by xi_lower.c.
 */

#include "xi_opt.h"
#include "xi_opt_gvn_pre.h"
#include "xi_tbaa.h"
#include "xi_effect.h"
#include "xi_opt_ifconv.h"
#include "xi_opt_inline.h"
#include "xi_opt_licm.h"
#include "xi_opt_loop_rotate.h"
#include "xi_opt_sccp.h"
#include "xi_opt_bce.h"
#include "xi_opt_block_simplify.h"
#include "xi_opt_jump_thread.h"
#include "xi_opt_strength.h"
#include "xi_opt_tail_call.h"
#include "xi_opt_ivsr.h"
#include "xi_opt_loop_peel.h"
#include "xi_opt_loop_unroll.h"
#include "xi_opt_loop_split.h"
#include "xi_opt_loop_inv_branch.h"
#include "xi_opt_comptime.h"
#include "xi_cfg_edit.h"
#include "xi_range.h"
#include "xi_value_query.h"
#include "../frontend/analyzer/xa_intrinsic_registry.h"
#include "../os/os_thread.h"
#include "../shared/xr_int_arith_core.h"
#include "../shared/xr_bits_core.h"
#include "../shared/xr_numeric_core.h"
#include "../shared/xr_null_test_core.h"
#include "../shared/xr_compare_core.h"
#include "xi_analysis.h"
#include "xi_analysis_manager.h"
#include "xi_edit.h"
#include "xi_evidence.h"
#include "xi_pass.h"
#include "xi_verify.h"
#include "xi_arc_verify.h"
#include "xi_coro_lower.h"
#include "../base/xdefs.h"
#include "../base/xglobal_indices.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../frontend/analyzer/xa_selection.h"
#include "../frontend/analyzer/xbuiltin_receiver_registry.h"
#include "../os/os_time.h"
#include "../runtime/symbol/xsymbol_table.h"
#include "../runtime/value/xtype.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ========== Helpers ========== */

/* Check if a value is a constant (integer or float). */
static bool is_const_int(const XiValue *v) {
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_INT;
}

static bool is_const_float(const XiValue *v) {
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_FLOAT;
}

static bool is_const_bool(const XiValue *v) {
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_BOOL;
}

static const XiValue *const_source_value(const XiValue *v) {
    while (v && xi_copy_is_identity_alias(v) && v->nargs >= 1)
        v = v->args[0];
    return v;
}

static bool const_int_value(const XiValue *v, int64_t *out) {
    v = const_source_value(v);
    if (!is_const_int(v))
        return false;
    if (out)
        *out = v->aux_int;
    return true;
}

static bool const_float_value(const XiValue *v, double *out) {
    v = const_source_value(v);
    if (!is_const_float(v))
        return false;
    if (out)
        memcpy(out, &v->aux_int, sizeof(double));
    return true;
}

static bool const_bool_value(const XiValue *v, bool *out) {
    v = const_source_value(v);
    if (!is_const_bool(v))
        return false;
    if (out)
        *out = v->aux_int != 0;
    return true;
}

static char *opt_arena_strdup(XiFunc *f, const char *s) {
    if (!f || !s)
        return NULL;
    size_t len = strlen(s);
    char *copy = (char *) xi_func_arena_alloc(f, (uint32_t) len + 1u);
    if (copy)
        memcpy(copy, s, len + 1u);
    return copy;
}

static XiConstLiteral *shared_const_literal_slot(XiFunc *f, int64_t slot) {
    if (slot < 0 || slot > UINT16_MAX)
        return NULL;
    for (XiFunc *cur = f; cur; cur = cur->parent_func) {
        uint16_t s = (uint16_t) slot;
        if (!cur->shared_const_literals || s >= cur->shared_const_literal_count)
            continue;
        if (!cur->slot_owned_consts || s >= cur->nshared || !cur->slot_owned_consts[s])
            continue;
        return &cur->shared_const_literals[s];
    }
    return NULL;
}

static bool const_literal_from_value(XiFunc *owner, const XiValue *v, XiConstLiteral *out) {
    if (!owner || !out)
        return false;
    v = const_source_value(v);
    if (!v || v->op != XI_CONST || !v->type)
        return false;

    memset(out, 0, sizeof(*out));
    out->type = v->type;
    switch (v->type->kind) {
        case XR_KIND_INT:
            out->kind = XI_CONST_LITERAL_INT;
            out->int_value = v->aux_int;
            return true;
        case XR_KIND_FLOAT:
            out->kind = XI_CONST_LITERAL_FLOAT;
            memcpy(&out->float_value, &v->aux_int, sizeof(double));
            return true;
        case XR_KIND_BOOL:
            out->kind = XI_CONST_LITERAL_BOOL;
            out->bool_value = v->aux_int != 0;
            return true;
        case XR_KIND_RUNE:
            out->kind = XI_CONST_LITERAL_CHAR;
            out->int_value = v->aux_int;
            return true;
        case XR_KIND_STRING:
            if (!v->aux)
                return false;
            out->kind = XI_CONST_LITERAL_STRING;
            out->string_value = opt_arena_strdup(owner, (const char *) v->aux);
            return out->string_value != NULL;
        case XR_KIND_NULL:
            out->kind = XI_CONST_LITERAL_NULL;
            return true;
        default:
            return false;
    }
}

static bool const_literal_equal(const XiConstLiteral *a, const XiConstLiteral *b) {
    if (!a || !b || a->kind != b->kind || a->type != b->type)
        return false;
    switch (a->kind) {
        case XI_CONST_LITERAL_NONE:
        case XI_CONST_LITERAL_NULL:
            return true;
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_CHAR:
            return a->int_value == b->int_value;
        case XI_CONST_LITERAL_FLOAT:
            return memcmp(&a->float_value, &b->float_value, sizeof(double)) == 0;
        case XI_CONST_LITERAL_BOOL:
            return a->bool_value == b->bool_value;
        case XI_CONST_LITERAL_STRING:
            return a->string_value && b->string_value &&
                   strcmp(a->string_value, b->string_value) == 0;
        default:
            return false;
    }
}

static bool record_shared_const_literal(XiFunc *f, int64_t slot, const XiValue *src) {
    XiConstLiteral *dst = shared_const_literal_slot(f, slot);
    if (!dst)
        return false;
    XiConstLiteral lit;
    if (!const_literal_from_value(f, src, &lit))
        return false;
    if (const_literal_equal(dst, &lit))
        return false;
    *dst = lit;
    return true;
}

XR_FUNC bool xi_rewrite_value_to_const_literal(XiValue *v, const XiConstLiteral *lit) {
    if (!v || !lit || lit->kind == XI_CONST_LITERAL_NONE)
        return false;
    switch (lit->kind) {
        case XI_CONST_LITERAL_NULL:
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_CHAR:
        case XI_CONST_LITERAL_FLOAT:
        case XI_CONST_LITERAL_BOOL:
        case XI_CONST_LITERAL_STRING:
            break;
        default:
            return false;
    }
    v->op = XI_CONST;
    /* Preserve the read's declared concrete type. A folded shared const must not
     * lose the width/signedness of the binding: e.g. a `uint64` const folded to
     * a signed `int` would make a later PTR_STORE into a uint64 slot fail verify.
     * Cross-module LTO separately recovers the export binding type because an
     * imported GET_SHARED may carry a tagged placeholder here. */
    if (!v->type || XR_TYPE_IS_UNKNOWN(v->type))
        v->type = lit->type;
    v->nargs = 0;
    v->aux = NULL;
    v->aux_int = 0;
    v->aux_kind = XI_AUX_KIND_NONE;
    v->flags = xi_op_default_effects(XI_CONST);
    v->mem_group = (uint8_t) xi_tbaa_group_for_op(v->op);
    v->xa_intrinsic_id = XA_INTRINSIC_NONE;
    switch (lit->kind) {
        case XI_CONST_LITERAL_NULL:
            return true;
        case XI_CONST_LITERAL_INT:
        case XI_CONST_LITERAL_CHAR:
            v->aux_int = lit->int_value;
            return true;
        case XI_CONST_LITERAL_FLOAT:
            memcpy(&v->aux_int, &lit->float_value, sizeof(double));
            return true;
        case XI_CONST_LITERAL_BOOL:
            v->aux_int = lit->bool_value ? 1 : 0;
            return true;
        case XI_CONST_LITERAL_STRING:
            v->aux = (void *) lit->string_value;
            return lit->string_value != NULL;
        default:
            return false;
    }
}

static bool opt_type_is_unsigned_int(const XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->is_nullable)
        return false;
    switch (type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static bool opt_type_is_int_like(const XrType *type) {
    return type && type->kind == XR_KIND_INT && !type->is_nullable;
}

static bool opt_compare_uses_unsigned(const XiValue *v) {
    if (!v || v->nargs < 2)
        return false;
    switch ((XiOp) v->op) {
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
            break;
        default:
            return false;
    }
    const XrType *left = v->args[0] ? v->args[0]->type : NULL;
    const XrType *right = v->args[1] ? v->args[1]->type : NULL;
    return opt_type_is_int_like(left) && opt_type_is_int_like(right) &&
           (opt_type_is_unsigned_int(left) || opt_type_is_unsigned_int(right));
}

/* Replace all uses of 'old_val' in the function with 'new_val'.
 * Scans all blocks, all values, all phi nodes. */
static void replace_all_uses(XiFunc *f, XiValue *old_val, XiValue *new_val) {
    XR_DCHECK(f != NULL, "replace_all_uses: NULL func");
    XR_DCHECK(old_val != NULL, "replace_all_uses: NULL old_val");
    XR_DCHECK(new_val != NULL, "replace_all_uses: NULL new_val");

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        /* Scan instructions */
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            bool args_changed = false;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == old_val) {
                    v->args[a] = new_val;
                    args_changed = true;
                }
            }
            if (args_changed)
                xi_value_rebase_view_evidence(v);
        }

        /* Scan phi nodes */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a] == old_val)
                    phi->value.args[a] = new_val;
            }
        }

        /* Scan block control value */
        if (blk->control == old_val)
            blk->control = new_val;
    }
}

/* Remove value at index 'idx' from block, shifting subsequent values. */
static void block_remove_value(XiBlock *blk, uint32_t idx) {
    XR_DCHECK(blk != NULL, "block_remove_value: NULL block");
    XR_DCHECK(idx < blk->nvalues, "block_remove_value: index out of bounds");

    for (uint32_t j = idx; j + 1 < blk->nvalues; j++)
        blk->values[j] = blk->values[j + 1];
    blk->nvalues--;
}

/* ========== Constant Folding ========== */

/* Try to fold a binary op on two integer constants.
 *
 * xray integer semantics: signed 64-bit, wrap-on-overflow (Go/Rust/Java).
 * C signed overflow is UB, so wrap arithmetic is performed via uint64_t and
 * cast back to int64_t (implementation-defined but well-defined on every
 * two's-complement target xray supports: x64, arm64, riscv64).
 * INT64_MIN / -1 and INT64_MIN %% -1 are special-cased to match the
 * runtime VM and AOT, which also produce INT64_MIN / 0 respectively.
 */
static bool fold_int_binary(uint16_t op, int64_t a, int64_t b, bool shr_unsigned,
                            bool divmod_unsigned, int64_t *result) {
    switch (op) {
        case XI_ADD:
            *result = xr_i64_add_wrap(a, b);
            return true;
        case XI_SUB:
            *result = xr_i64_sub_wrap(a, b);
            return true;
        case XI_MUL:
            *result = xr_i64_mul_wrap(a, b);
            return true;
        case XI_DIV:
            if (b == 0)
                return false;
            *result = xr_int_div_mod_apply(divmod_unsigned ? XR_INT_DIV_MOD_DIV_U
                                                           : XR_INT_DIV_MOD_DIV,
                                           XR_INT_DIV_MOD_PROOF_NONZERO, a, b);
            return true;
        case XI_MOD:
            if (b == 0)
                return false;
            *result = xr_int_div_mod_apply(divmod_unsigned ? XR_INT_DIV_MOD_MOD_U
                                                           : XR_INT_DIV_MOD_MOD,
                                           XR_INT_DIV_MOD_PROOF_NONZERO, a, b);
            return true;
        case XI_BAND:
            *result = xr_bitwise_binary_i64(XR_BITWISE_BINARY_AND, a, b);
            return true;
        case XI_BOR:
            *result = xr_bitwise_binary_i64(XR_BITWISE_BINARY_OR, a, b);
            return true;
        case XI_BXOR:
            *result = xr_bitwise_binary_i64(XR_BITWISE_BINARY_XOR, a, b);
            return true;
        case XI_SHL:
            *result = xr_shift_i64(XR_SHIFT_LEFT, a, b);
            return true;
        case XI_SHR:
            *result = xr_shift_i64(shr_unsigned ? XR_SHIFT_RIGHT_UNSIGNED
                                                : XR_SHIFT_RIGHT_SIGNED,
                                   a, b);
            return true;
        default:
            return false;
    }
}

/* Constant folding answers the same relation the executors do, through the same
 * owner: a fold that disagreed with the runtime would move the answer into the
 * compiler. */
#define XI_OPT_COMPARE_I64(kind, a, b)                                                             \
    XR_COMPARE_OWNER_APPLY_I64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_SEMANTIC_PLAN,   \
                               (kind), (a), (b))
#define XI_OPT_COMPARE_U64(kind, a, b)                                                             \
    XR_COMPARE_OWNER_APPLY_U64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_SEMANTIC_PLAN,   \
                               (kind), (a), (b))
#define XI_OPT_COMPARE_F64(kind, a, b)                                                             \
    XR_COMPARE_OWNER_APPLY_F64(XR_SEM_OWNER_ID_SHARED_COMPARE_HI,                                  \
                               XR_SEM_OWNER_ID_SHARED_COMPARE_LO, XR_SEM_CONSUMER_SEMANTIC_PLAN,   \
                               (kind), (a), (b))

/* The Xi opcode that carries each relation. */
static bool xi_compare_kind(uint16_t op, XrCompareKind *kind) {
    switch (op) {
        case XI_EQ:
            *kind = XR_COMPARE_EQ;
            return true;
        case XI_NE:
            *kind = XR_COMPARE_NE;
            return true;
        case XI_LT:
            *kind = XR_COMPARE_LT;
            return true;
        case XI_LE:
            *kind = XR_COMPARE_LE;
            return true;
        case XI_GT:
            *kind = XR_COMPARE_GT;
            return true;
        case XI_GE:
            *kind = XR_COMPARE_GE;
            return true;
        default:
            return false;
    }
}

/* Try to fold a comparison on two integer constants. */
static bool fold_int_compare(uint16_t op, int64_t a, int64_t b, bool use_unsigned, bool *result) {
    XrCompareKind kind;
    if (!xi_compare_kind(op, &kind))
        return false;
    /* Equality reads the same bits either way, so only the order relations need
     * the unsigned lane. */
    if (use_unsigned && !xr_compare_kind_is_equality_core(kind))
        *result = XI_OPT_COMPARE_U64(kind, (uint64_t) a, (uint64_t) b);
    else
        *result = XI_OPT_COMPARE_I64(kind, a, b);
    return true;
}

/* Try to fold a binary op on two float constants.
 *
 * When the result is float32, narrow each operand and the result to single
 * precision so the fold matches the VM *_F32 opcodes and AOT codegen exactly;
 * folding float32 arithmetic in double would round once instead of per-operand
 * and diverge from the runtime. */
static bool fold_float_binary(uint16_t op, double a, double b, bool is_f32, double *result) {
    if (is_f32) {
        float fa = (float) a, fb = (float) b;
        switch (op) {
            case XI_ADD:
                *result = (double) (float) (fa + fb);
                return true;
            case XI_SUB:
                *result = (double) (float) (fa - fb);
                return true;
            case XI_MUL:
                *result = (double) (float) (fa * fb);
                return true;
            case XI_DIV:
                /* f32 division: narrowed operands, divide in double, narrow the
                 * quotient back to float (matches OP_DIV_F32 / AOT). */
                *result = (double) (float) ((double) fa / (double) fb);
                return true;
            default:
                return false;
        }
    }
    switch (op) {
        case XI_ADD:
            *result = a + b;
            return true;
        case XI_SUB:
            *result = a - b;
            return true;
        case XI_MUL:
            *result = a * b;
            return true;
        case XI_DIV:
            *result = a / b;
            return true;
        default:
            return false;
    }
}

/* Try to fold a comparison on two float constants. */
static bool fold_float_compare(uint16_t op, double a, double b, bool *result) {
    XrCompareKind kind;
    if (!xi_compare_kind(op, &kind))
        return false;
    *result = XI_OPT_COMPARE_F64(kind, a, b);
    return true;
}

static void rewrite_to_const_int(XiValue *v, int64_t value) {
    XR_DCHECK(v != NULL, "rewrite_to_const_int: NULL value");
    v->op = XI_CONST;
    v->aux_int = value;
    v->aux = NULL;
    v->aux_kind = XI_AUX_KIND_NONE;
    v->nargs = 0;
    v->flags = xi_op_default_effects(XI_CONST);
    v->mem_group = (uint8_t) xi_tbaa_group_for_op(v->op);
    v->xa_intrinsic_id = XA_INTRINSIC_NONE;
}

static bool ordering_member_index_opt(const char *name, int64_t *out_index) {
    if (!name || !out_index)
        return false;
    if (strcmp(name, "Relaxed") == 0) {
        *out_index = 0;
        return true;
    }
    if (strcmp(name, "Acquire") == 0) {
        *out_index = 1;
        return true;
    }
    if (strcmp(name, "Release") == 0) {
        *out_index = 2;
        return true;
    }
    if (strcmp(name, "AcquireRelease") == 0) {
        *out_index = 3;
        return true;
    }
    if (strcmp(name, "SeqCst") == 0) {
        *out_index = 4;
        return true;
    }
    return false;
}

static bool rewrite_ordering_member_to_const_int(XiValue *v) {
    if (!v || v->op != XI_LOAD_FIELD || v->nargs < 1 || !v->args[0] || !v->aux)
        return false;
    XiValue *recv = v->args[0];
    while (recv && xi_copy_is_identity_alias(recv) && recv->nargs >= 1)
        recv = recv->args[0];
    if (!recv || recv->op != XI_GET_BUILTIN || recv->aux_int != XR_GLOBAL_VAR_ORDERING)
        return false;
    int64_t index = 0;
    if (!ordering_member_index_opt((const char *) v->aux, &index))
        return false;
    rewrite_to_const_int(v, index);
    v->type = xr_type_new_int(NULL);
    return true;
}

static void rewrite_to_const_float(XiValue *v, double value) {
    XR_DCHECK(v != NULL, "rewrite_to_const_float: NULL value");
    v->op = XI_CONST;
    memcpy(&v->aux_int, &value, sizeof(double));
    v->aux = NULL;
    v->aux_kind = XI_AUX_KIND_NONE;
    v->nargs = 0;
    v->flags = xi_op_default_effects(XI_CONST);
    v->mem_group = (uint8_t) xi_tbaa_group_for_op(v->op);
    v->xa_intrinsic_id = XA_INTRINSIC_NONE;
}

static void rewrite_to_copy(XiValue *v, XiValue *src) {
    XR_DCHECK(v != NULL, "rewrite_to_copy: NULL value");
    XR_DCHECK(src != NULL, "rewrite_to_copy: NULL source");
    v->op = XI_COPY;
    v->args[0] = src;
    v->nargs = 1;
    v->flags = xi_op_default_effects(XI_COPY);
    v->aux_int = XI_COPY_KIND_IDENTITY;
    v->aux = NULL;
    v->aux_kind = XI_AUX_KIND_NONE;
    v->mem_group = (uint8_t) xi_tbaa_group_for_op(v->op);
    v->xa_intrinsic_id = XA_INTRINSIC_NONE;
}

static bool fold_exact_integer_unary(XiValue *v, int64_t operand) {
    int64_t result;
    switch (v->op) {
        case XI_BIT_BSWAP:
            result = xr_bits_exact_byteswap(operand, (uint8_t) v->aux_int);
            break;
        case XI_BIT_POPCOUNT:
            result = xr_bits_exact_popcount(operand, (uint8_t) v->aux_int);
            break;
        case XI_BIT_CLZ:
            result = xr_bits_exact_leading_zeros(operand, (uint8_t) v->aux_int);
            break;
        case XI_BIT_CTZ:
            result = xr_bits_exact_trailing_zeros(operand, (uint8_t) v->aux_int);
            break;
        default:
            return false;
    }
    rewrite_to_const_int(v, result);
    return true;
}

static bool fold_exact_integer_binary(XiValue *v, int64_t lhs, int64_t rhs) {
    int64_t result;
    if (v->op == XI_BIT_ROTL || v->op == XI_BIT_ROTR) {
        result = v->op == XI_BIT_ROTL ? xr_bits_exact_rotate_left(lhs, rhs, (uint8_t) v->aux_int)
                                      : xr_bits_exact_rotate_right(lhs, rhs, (uint8_t) v->aux_int);
    } else if (v->op == XI_BIT_MUL_HIGH) {
        unsigned bits = v->aux_int == XR_NATIVE_U8      ? 8u
                        : v->aux_int == XR_NATIVE_U16   ? 16u
                        : v->aux_int == XR_NATIVE_U32   ? 32u
                        : v->aux_int == XR_NATIVE_USIZE ? (unsigned) (sizeof(uintptr_t) * 8u)
                                                        : 64u;
        result = (int64_t) xr_uint_mul_high_bits((uint64_t) lhs, (uint64_t) rhs, bits);
    } else {
        return false;
    }
    rewrite_to_const_int(v, result);
    return true;
}

static bool enum_metadata_value_token(const XiValue *v, int64_t *out_token) {
    if (!v || !out_token)
        return false;
    const XrType *owner =
        v->enum_metadata_owner ? v->enum_metadata_owner : xr_type_enum_metadata_owner(v->type);
    XrEnumMetadataKind kind = v->enum_metadata_kind != XR_ENUM_METADATA_NONE
                                  ? (XrEnumMetadataKind) v->enum_metadata_kind
                                  : xr_type_enum_metadata_kind(v->type);
    if (!owner || owner->kind != XR_KIND_ENUM || !owner->enum_type.layout ||
        owner->enum_type.layout->layout_id == 0 || kind == XR_ENUM_METADATA_NONE)
        return false;
    *out_token = ((int64_t) owner->enum_type.layout->layout_id << 8) | (int64_t) kind;
    return true;
}

/* A direct unit-enum for-in loop lowers its induction variable as
 *
 *     INDEX_GET(enum-domain, ordinal)  [XI_AUX_KIND_ENUM_CASE]
 *
 * The value is real and may escape as an enum, but projecting `.ordinal`
 * immediately after that access must not materialize one static enum box per
 * declaration.  The verified enum-domain access is indexed by exactly the
 * declaration ordinal, so the projection is the index itself.  Rewriting here
 * also lets DCE remove the otherwise O(variant-count) AOT switch when the loop
 * only needs the tag. */
static XiValue *enum_case_iteration_ordinal(XiValue *value) {
    for (uint8_t depth = 0; value && depth < 8; depth++) {
        if (value->op == XI_INDEX_GET && value->aux_kind == XI_AUX_KIND_ENUM_CASE &&
            value->nargs >= 2 && value->args[1] && value->type &&
            value->type->kind == XR_KIND_ENUM && value->type->enum_type.layout &&
            value->type->enum_type.layout->is_zero_payload)
            return value->args[1];
        if (!xi_copy_is_identity_alias(value) || value->nargs < 1 || !value->args[0])
            return NULL;
        value = value->args[0];
    }
    return NULL;
}

static bool fold_enum_metadata_value(XiValue *value) {
    if (value->op == XI_IS && value->nargs == 2 && value->args[0] && value->args[1]) {
        int64_t source_token = 0;
        int64_t target_token = 0;
        if (enum_metadata_value_token(value->args[0], &source_token) &&
            const_int_value(value->args[1], &target_token)) {
            rewrite_to_const_int(value, source_token == target_token ? 1 : 0);
            return true;
        }
    }

    if (value->op == XI_LOAD_FIELD && value->nargs == 1 &&
        value->enum_metadata_field == XA_ENUM_META_ORDINAL) {
        XiValue *ordinal = enum_case_iteration_ordinal(value->args[0]);
        if (ordinal) {
            rewrite_to_copy(value, ordinal);
            return true;
        }
    }
    return false;
}

static bool fold_tuple_projection_value(XiValue *value) {
    if (!value || value->op != XI_TUPLE_GET || value->nargs != 1 || !value->args[0] ||
        value->args[0]->op != XI_TUPLE_NEW)
        return false;
    XiValue *tuple = value->args[0];
    int64_t index = value->aux_int;
    if (index < 0 || (uint16_t) index >= tuple->nargs || !tuple->args[(uint16_t) index])
        return false;
    rewrite_to_copy(value, tuple->args[(uint16_t) index]);
    return true;
}

XR_FUNC XiPassChange xi_opt_const_fold(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_const_fold: NULL func");
    XiPassChange chg = xi_pass_no_change();

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];

            if (rewrite_ordering_member_to_const_int(v)) {
                chg.values_changed = true;
                continue;
            }

            if (v->op == XI_GET_SHARED) {
                const XiConstLiteral *lit = shared_const_literal_slot(f, v->aux_int);
                if (lit && !lit->data_weak && xi_rewrite_value_to_const_literal(v, lit)) {
                    chg.values_changed = true;
                    continue;
                }
            }

            if (v->op == XI_SET_SHARED && v->nargs == 1) {
                if (record_shared_const_literal(f, v->aux_int, v->args[0]))
                    chg.values_changed = true;
                continue;
            }

            /* Fold unary NEG on const int.
             * -INT64_MIN is UB on signed; negate on uint64_t then cast back
             * to preserve wrap-on-overflow semantics (matches VM and AOT). */
            int64_t unary_i = 0;
            if (v->op == XI_NEG && v->nargs == 1 && const_int_value(v->args[0], &unary_i)) {
                rewrite_to_const_int(v, xr_numeric_neg_eval(XR_NUMERIC_NEG_I64, unary_i, 0.0).i64);
                chg.values_changed = true;
                continue;
            }

            /* Fold unary NEG on const float */
            double unary_f = 0.0;
            if (v->op == XI_NEG && v->nargs == 1 && const_float_value(v->args[0], &unary_f)) {
                rewrite_to_const_float(
                    v, xr_numeric_neg_eval(XR_NUMERIC_NEG_F64, 0, unary_f).f64);
                chg.values_changed = true;
                continue;
            }

            /* Fold unary NOT on const bool */
            bool unary_b = false;
            if (v->op == XI_NOT && v->nargs == 1 && const_bool_value(v->args[0], &unary_b)) {
                rewrite_to_const_int(v, unary_b ? 0 : 1);
                chg.values_changed = true;
                continue;
            }

            /* Fold unary BNOT on const int */
            if (v->op == XI_BNOT && v->nargs == 1 && const_int_value(v->args[0], &unary_i)) {
                rewrite_to_const_int(v, xr_bits_not_i64(unary_i));
                chg.values_changed = true;
                continue;
            }

            if (v->nargs == 1 && const_int_value(v->args[0], &unary_i) &&
                fold_exact_integer_unary(v, unary_i)) {
                chg.values_changed = true;
                continue;
            }

            /* Descriptor type tests and unit-enum ordinal projections are
             * fully static and must not force descriptor/value materialization. */
            if (fold_enum_metadata_value(v)) {
                chg.values_changed = true;
                continue;
            }

            /* Tuple projection: TUPLE_GET(TUPLE_NEW(e0..en-1), idx) → COPY(e_idx).
             * Tuples are immutable, so the source slot is always the literal
             * element passed at construction time.  Out-of-range indices are
             * impossible: the analyzer rejects them and the verifier asserts
             * arity, so reaching this peephole with idx >= nargs would be a
             * compiler bug — leave the GET intact and let later stages fail
             * loudly. */
            if (fold_tuple_projection_value(v)) {
                chg.values_changed = true;
                continue;
            }

            /* Binary: need exactly 2 args */
            if (v->nargs != 2)
                continue;
            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];

            /* Integer binary/compare */
            int64_t lhs_i = 0, rhs_i = 0;
            if (const_int_value(lhs, &lhs_i) && const_int_value(rhs, &rhs_i)) {
                int64_t result;
                if (fold_exact_integer_binary(v, lhs_i, rhs_i)) {
                    chg.values_changed = true;
                    continue;
                }
                bool shr_unsigned = v->op == XI_SHR && opt_type_is_int_like(lhs->type) &&
                                    opt_type_is_unsigned_int(lhs->type);
                /* Result-type check recovers the unsigned intent when inlining
                 * has substituted integer literals for typed uint params (the
                 * operand types lose their signedness, but the div node's own
                 * type stays uint). Result-unsigned implies operand-unsigned in
                 * well-typed code, so this cannot over-trigger vs the VM. */
                bool divmod_unsigned =
                    (v->op == XI_DIV || v->op == XI_MOD) &&
                    (opt_type_is_unsigned_int(v->type) ||
                     (opt_type_is_int_like(lhs->type) && opt_type_is_int_like(rhs->type) &&
                      (opt_type_is_unsigned_int(lhs->type) ||
                       opt_type_is_unsigned_int(rhs->type))));
                if (fold_int_binary(v->op, lhs_i, rhs_i, shr_unsigned, divmod_unsigned, &result)) {
                    rewrite_to_const_int(v, result);
                    chg.values_changed = true;
                    continue;
                }
                bool bres;
                if (fold_int_compare(v->op, lhs_i, rhs_i, opt_compare_uses_unsigned(v), &bres)) {
                    rewrite_to_const_int(v, bres ? 1 : 0);
                    chg.values_changed = true;
                    continue;
                }
            }

            /* Float binary/compare */
            double lhs_f = 0.0, rhs_f = 0.0;
            if (const_float_value(lhs, &lhs_f) && const_float_value(rhs, &rhs_f)) {
                double dresult;
                bool is_f32 = v->type && v->type->kind == XR_KIND_FLOAT &&
                              v->type->scalar_rep == XR_NATIVE_F32;
                if (fold_float_binary(v->op, lhs_f, rhs_f, is_f32, &dresult)) {
                    rewrite_to_const_float(v, dresult);
                    chg.values_changed = true;
                    continue;
                }
                bool bres;
                if (fold_float_compare(v->op, lhs_f, rhs_f, &bres)) {
                    rewrite_to_const_int(v, bres ? 1 : 0);
                    chg.values_changed = true;
                    continue;
                }
            }
        }
    }
    return chg;
}

/* ========== Copy Propagation ========== */

/* Resolve through XI_COPY chains to find the original source.
 * Stops at variable domain boundaries: when a COPY's var_id differs
 * from its source's var_id, the copy separates two coalescing domains
 * (e.g. `var temp = b`).  Resolving through it would merge the
 * domains, causing loop-carried variables to share a physical
 * register and corrupt each other on reassignment. */
static XiValue *resolve_copy(XiValue *v) {
    while (v && xi_copy_is_identity_alias(v) && v->nargs >= 1) {
        XiValue *src = v->args[0];
        /* Stop at variable-domain boundaries (prevents register corruption) */
        if (xi_var_id_is_valid(v->var_id) && src && xi_var_id_is_valid(src->var_id) &&
            v->var_id != src->var_id)
            break;
        /* Stop at type-view boundaries. Nullable narrowing and native-width
         * narrowing carry semantic type information even when the runtime
         * payload is the same value. */
        if (v->type && src && src->type && !xr_type_equals(v->type, src->type))
            break;
        v = src;
    }
    return v;
}

XR_FUNC XiPassChange xi_opt_copy_prop(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_copy_prop: NULL func");
    XiPassChange chg = xi_pass_no_change();

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        /* Rewrite args of each value */
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            bool args_changed = false;
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *resolved = resolve_copy(v->args[a]);
                if (resolved && resolved != v->args[a]) {
                    v->args[a] = resolved;
                    args_changed = true;
                    chg.values_changed = true;
                }
            }
            if (args_changed)
                xi_value_rebase_view_evidence(v);
        }

        /* Rewrite phi operands — but preserve variable-boundary copies.
         * A COPY with a var_id different from its source separates two
         * coalescing domains (e.g. from `x = i`).  Resolving it would
         * merge the domains and corrupt phi moves at loop back-edges. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiValue *arg = phi->value.args[a];
                if (!arg || arg->op != XI_COPY || arg->nargs < 1)
                    continue;
                XiValue *resolved = resolve_copy(arg);
                if (resolved && resolved != arg &&
                    (!xi_var_id_is_valid(arg->var_id) || arg->var_id == resolved->var_id)) {
                    phi->value.args[a] = resolved;
                    chg.values_changed = true;
                }
            }
        }

        /* Rewrite block control */
        if (blk->control) {
            XiValue *resolved = resolve_copy(blk->control);
            if (resolved && resolved != blk->control) {
                blk->control = resolved;
                chg.values_changed = true;
            }
        }
    }
    return chg;
}

/* ========== Dead Code Elimination ========== */

/* Compute use counts for all values in the function.
 * Initializes all uses to 0, then increments for each reference. */
static void compute_use_counts(XiFunc *f) {
    /* Reset all use counts */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++)
            blk->values[i]->uses = 0;
        for (XiPhi *phi = blk->phis; phi; phi = phi->next)
            phi->value.uses = 0;
    }

    /* Count uses from instruction operands */
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a])
                    v->args[a]->uses++;
            }
        }

        /* Uses from phi operands */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                if (phi->value.args[a])
                    phi->value.args[a]->uses++;
            }
        }

        /* Use from block control */
        if (blk->control)
            blk->control->uses++;
    }
}

static XiValue *find_unique_arg_user(XiFunc *f, XiValue *needle) {
    if (!f || !needle)
        return NULL;
    XiValue *found = NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] != needle)
                    continue;
                if (found)
                    return NULL;
                found = v;
            }
        }
    }
    return found;
}

static bool xi_await_is_plain_one_task(const XiValue *v) {
    if (!v || v->op != XI_AWAIT || v->nargs != 1)
        return false;
    const int64_t variant_bits = XI_AWAIT_AUX_ANY | XI_AWAIT_AUX_ALL | XI_AWAIT_AUX_ANY_SUCCESS;
    return (v->aux_int & variant_bits) == 0;
}

static bool xi_await_is_all_tasks(const XiValue *v) {
    if (!v || v->op != XI_AWAIT || v->nargs < 1)
        return false;
    return (v->aux_int & XI_AWAIT_AUX_ALL) != 0 &&
           (v->aux_int & (XI_AWAIT_AUX_ANY | XI_AWAIT_AUX_ANY_SUCCESS)) == 0;
}

static bool xi_go_can_be_one_shot_awaited(const XiValue *v) {
    if (!v || v->op != XI_GO)
        return false;
    if (v->flags & XI_FLAG_FIRE_AND_FORGET)
        return false;
    /* A named Task with a compiler-published shared Copy result preserves the
     * source multi-observer contract.  Only the direct-temporary lowering path
     * may consume such a handle; use-count optimization must not reclassify it
     * as a unique-result Task. */
    if ((v->aux_int & XI_GO_AUX_RESULT_COPY_SHARED) != 0)
        return false;
    return (v->aux_int & XI_GO_AUX_LINK_MASK) == 0;
}

static bool xi_identity_keeps_task_view(const XiValue *from, const XiValue *to) {
    if (!from || !to || (to->op != XI_COPY && !xi_op_is_identity_forward(to->op)) ||
        to->nargs != 1 || to->args[0] != from)
        return false;
    if (!from->type || !to->type)
        return true;
    return xr_type_equals(from->type, to->type);
}

static XiValue *find_one_shot_await_for_go(XiFunc *f, XiValue *go) {
    XiValue *cur = go;
    for (uint8_t depth = 0; depth < 8; depth++) {
        if (!cur || cur->uses != 1)
            return NULL;
        XiValue *user = find_unique_arg_user(f, cur);
        if (!user)
            return NULL;
        if (xi_await_is_plain_one_task(user) && user->args[0] == cur)
            return user;
        if (xi_identity_keeps_task_view(cur, user)) {
            cur = user;
            continue;
        }
        return NULL;
    }
    return NULL;
}

static XiValue *unwrap_unique_task_go_for_aggregate_set(XiValue *value) {
    XiValue *cur = value;
    for (uint8_t depth = 0; depth < 8; depth++) {
        if (!cur || cur->uses != 1)
            return NULL;
        if (cur->op == XI_GO)
            return xi_go_can_be_one_shot_awaited(cur) ? cur : NULL;
        if (cur->nargs == 1 && xi_identity_keeps_task_view(cur->args[0], cur)) {
            cur = cur->args[0];
            continue;
        }
        return NULL;
    }
    return NULL;
}

static bool xi_opt_method_name_is(const XiValue *v, const char *name, SymbolId symbol) {
    if (!v || !name)
        return false;
    const char *method = v->aux ? (const char *) v->aux : NULL;
    if (method && strcmp(method, name) == 0)
        return true;
    if (v->aux_int <= 0)
        return false;
    return (SymbolId) (v->aux_int >> 1) == symbol;
}

static const XiValue *xi_identity_root_value(const XiValue *v) {
    const XiValue *cur = v;
    for (uint8_t depth = 0; depth < 8; depth++) {
        if (!cur || cur->nargs != 1)
            return cur;
        const XiValue *src = cur->args[0];
        if (!xi_identity_keeps_task_view(src, cur))
            return cur;
        cur = src;
    }
    return cur;
}

static XiValue *xi_task_array_unwrap_identity_depth(XiValue *v, uint8_t depth) {
    XiValue *cur = v;
    for (; depth < 8; depth++) {
        if (!cur || cur->nargs != 1)
            break;
        XiValue *src = cur->args[0];
        if (!xi_identity_keeps_task_view(src, cur))
            break;
        cur = src;
    }

    if (!cur || cur->op != XI_PHI || depth >= 8)
        return cur;

    XiValue *source = NULL;
    for (uint16_t i = 0; i < cur->nargs; i++) {
        XiValue *arg = cur->args[i];
        if (!arg || arg == cur)
            continue;
        XiValue *root = xi_task_array_unwrap_identity_depth(arg, depth + 1);
        if (!root || root == cur)
            continue;
        if (source && source != root)
            return cur;
        source = root;
    }
    if (!source)
        return cur;
    if (cur->type && source->type && !xr_type_equals(cur->type, source->type))
        return cur;
    return source;
}

static XiValue *xi_task_array_unwrap_identity(XiValue *v) {
    return xi_task_array_unwrap_identity_depth(v, 0);
}

static XiValue *xi_task_array_unique_shared_init(XiFunc *f, int64_t slot) {
    if (!f || slot < 0)
        return NULL;
    XiValue *init = NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        if (!blk)
            continue;
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_SET_SHARED || v->aux_int != slot || v->nargs < 1)
                continue;
            XiValue *rhs = xi_task_array_unwrap_identity(v->args[0]);
            if (!rhs || rhs->op != XI_ARRAY_NEW)
                return NULL;
            if (init && init != rhs)
                return NULL;
            init = rhs;
        }
    }
    return init;
}

static bool xi_task_array_root_is_known(XiFunc *f, XiValue *v) {
    v = xi_task_array_unwrap_identity(v);
    if (!v)
        return false;
    if (v->op == XI_ARRAY_NEW)
        return true;
    return v->op == XI_GET_SHARED && xi_task_array_unique_shared_init(f, v->aux_int) != NULL;
}

static bool xi_task_array_values_same(XiFunc *f, XiValue *a, XiValue *b) {
    a = xi_task_array_unwrap_identity(a);
    b = xi_task_array_unwrap_identity(b);
    if (!a || !b)
        return false;
    if (a == b)
        return true;
    if (a->op == XI_GET_SHARED && b->op == XI_GET_SHARED && a->aux_int == b->aux_int)
        return xi_task_array_unique_shared_init(f, a->aux_int) != NULL;
    if (a->op == XI_GET_SHARED && b->op == XI_ARRAY_NEW)
        return xi_task_array_unique_shared_init(f, a->aux_int) == b;
    if (b->op == XI_GET_SHARED && a->op == XI_ARRAY_NEW)
        return xi_task_array_unique_shared_init(f, b->aux_int) == a;
    return false;
}

static bool xi_task_array_use_is_shared_init(XiFunc *f, XiValue *user, uint16_t arg_idx,
                                             XiValue *arr) {
    if (!f || !user || !arr || user->op != XI_SET_SHARED || user->nargs < 1 || arg_idx != 0)
        return false;
    XiValue *root = xi_task_array_unwrap_identity(arr);
    if (root && root->op == XI_GET_SHARED && root->aux_int == user->aux_int)
        return xi_task_array_unique_shared_init(f, root->aux_int) ==
               xi_task_array_unwrap_identity(user->args[0]);
    if (root && root->op == XI_ARRAY_NEW)
        return xi_task_array_unique_shared_init(f, user->aux_int) == root;
    return false;
}

static bool xi_result_group_recv_uses_group(const XiValue *v, const XiValue *group) {
    if (!v || !group || v->nargs < 1)
        return false;
    if (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT)
        return false;
    if (!xi_opt_method_name_is(v, "recv", SYMBOL_RECV))
        return false;
    if (!xi_value_type_is_result_group(v->args[0]))
        return false;
    return xi_identity_root_value(v->args[0]) == xi_identity_root_value(group);
}

static bool xi_func_has_result_group_recv_for_group(XiFunc *f, const XiValue *group) {
    if (!f || !group)
        return false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            if (xi_result_group_recv_uses_group(blk->values[i], group))
                return true;
        }
    }
    return false;
}

static bool xi_go_can_defer_fire_and_forget_result_group(XiFunc *f, const XiValue *go) {
    if (!f || !go || go->op != XI_GO)
        return false;
    if ((go->flags & XI_FLAG_FIRE_AND_FORGET) == 0)
        return false;
    if ((go->aux_int & XI_GO_AUX_LINK_MASK) != 0)
        return false;
    for (uint16_t a = 1; a < go->nargs; a++) {
        XiValue *arg = go->args[a];
        if (xi_value_type_is_result_group(arg) && xi_func_has_result_group_recv_for_group(f, arg))
            return true;
    }
    return false;
}

static bool xi_mark_fire_and_forget_result_group_go_deferred(XiFunc *f, XiValue *go,
                                                             XiPassChange *chg) {
    if (!xi_go_can_defer_fire_and_forget_result_group(f, go))
        return false;
    if ((go->aux_int & XI_GO_AUX_DEFER_BATCH) != 0)
        return false;
    go->aux_int |= XI_GO_AUX_DEFER_BATCH;
    if (chg) {
        chg->values_changed = true;
        chg->n_added++;
    }
    return true;
}

static bool xi_await_all_task_array_pushes_go(XiFunc *f, XiValue *user, XiValue *arr) {
    if (!user || !arr)
        return false;
    if (user->op == XI_INDEX_SET && user->nargs >= 3 &&
        xi_task_array_values_same(f, user->args[0], arr))
        return unwrap_unique_task_go_for_aggregate_set(user->args[2]) != NULL;
    if ((user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) && user->nargs >= 2 &&
        xi_task_array_values_same(f, user->args[0], arr) &&
        xi_opt_method_name_is(user, "push", SYMBOL_PUSH)) {
        return unwrap_unique_task_go_for_aggregate_set(user->args[1]) != NULL;
    }
    return false;
}

static XiValue *xi_await_all_task_array_pushed_go(XiFunc *f, XiValue *user, XiValue *arr) {
    if (!xi_await_all_task_array_pushes_go(f, user, arr))
        return NULL;
    if (user->op == XI_INDEX_SET)
        return unwrap_unique_task_go_for_aggregate_set(user->args[2]);
    return unwrap_unique_task_go_for_aggregate_set(user->args[1]);
}

static bool xi_await_all_task_array_allowed_structural_use(XiFunc *f, const XiValue *user,
                                                           uint16_t arg_idx, XiValue *arr) {
    if (!user || !arr || arg_idx != 0)
        return false;
    if (user->op == XI_CALL_BUILTIN && user->aux) {
        const char *name = (const char *) user->aux;
        return strcmp(name, "array_clear") == 0 || strcmp(name, "array_reserve") == 0;
    }
    if ((user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) &&
        xi_task_array_values_same(f, user->args[0], arr)) {
        return xi_opt_method_name_is(user, "clear", SYMBOL_CLEAR) ||
               xi_opt_method_name_is(user, "reserve", SYMBOL_RESERVE);
    }
    return false;
}

static bool xi_task_array_use_is_ownership_bookkeeping(const XiValue *user, uint16_t arg_idx) {
    return user && arg_idx == 0 && (user->op == XI_RETAIN || user->op == XI_RELEASE);
}

static bool xi_task_array_use_is_clear(XiFunc *f, const XiValue *user, uint16_t arg_idx,
                                       XiValue *arr) {
    if (!user || !arr || arg_idx != 0)
        return false;
    if (user->op == XI_CALL_BUILTIN && user->aux) {
        const char *name = (const char *) user->aux;
        return strcmp(name, "array_clear") == 0;
    }
    if ((user->op == XI_CALL_METHOD || user->op == XI_CALL_METHOD_DIRECT) &&
        xi_task_array_values_same(f, user->args[0], arr)) {
        return xi_opt_method_name_is(user, "clear", SYMBOL_CLEAR);
    }
    return false;
}

static bool xi_await_all_fresh_task_array_can_be_one_shot(XiFunc *f, XiValue *await, XiValue *arr) {
    if (!f || !xi_await_is_all_tasks(await) || !xi_task_array_root_is_known(f, arr))
        return false;

    bool saw_set = false;
    bool saw_await = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (!xi_task_array_values_same(f, v->args[a], arr))
                    continue;
                if (v == await && a == 0) {
                    if (saw_await)
                        return false;
                    saw_await = true;
                    continue;
                }
                if (xi_task_array_use_is_shared_init(f, v, a, arr))
                    continue;
                if (xi_task_array_use_is_ownership_bookkeeping(v, a))
                    continue;
                if (a == 0 && xi_await_all_task_array_pushes_go(f, v, arr)) {
                    saw_set = true;
                    continue;
                }
                if (xi_await_all_task_array_allowed_structural_use(f, v, a, arr))
                    continue;
                return false;
            }
        }
    }
    return saw_set && saw_await;
}

static bool xi_mark_await_all_fresh_task_array_one_shot(XiFunc *f, XiValue *await, XiValue *arr,
                                                        XiPassChange *chg) {
    if (!xi_await_all_fresh_task_array_can_be_one_shot(f, await, arr))
        return false;

    bool changed = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->nargs < 2)
                continue;
            XiValue *go = xi_await_all_task_array_pushed_go(f, v, arr);
            if (!go)
                continue;
            if ((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0) {
                go->aux_int |= XI_GO_AUX_ONE_SHOT_AWAIT;
                changed = true;
            }
            if ((go->aux_int & XI_GO_AUX_DEFER_BATCH) == 0) {
                go->aux_int |= XI_GO_AUX_DEFER_BATCH;
                changed = true;
            }
        }
    }

    if ((await->aux_int & XI_AWAIT_AUX_AGGREGATE_ONE_SHOT) == 0) {
        await->aux_int |= XI_AWAIT_AUX_AGGREGATE_ONE_SHOT;
        changed = true;
    }

    if (changed && chg) {
        chg->values_changed = true;
        chg->n_added++;
    }
    return changed;
}

static XiValue *xi_plain_await_for_task_index_get(XiFunc *f, XiValue *index_get) {
    XiValue *cur = index_get;
    for (uint8_t depth = 0; depth < 8; depth++) {
        if (!cur || cur->uses != 1)
            return NULL;
        XiValue *user = find_unique_arg_user(f, cur);
        if (!user)
            return NULL;
        if (xi_await_is_plain_one_task(user) && user->args[0] == cur)
            return user;
        if (xi_identity_keeps_task_view(cur, user)) {
            cur = user;
            continue;
        }
        return NULL;
    }
    return NULL;
}

static bool xi_plain_await_consumes_task_index_get(XiFunc *f, XiValue *index_get) {
    return xi_plain_await_for_task_index_get(f, index_get) != NULL;
}

static const XiValue *xi_task_loop_unwrap_identity(const XiValue *v) {
    const XiValue *cur = v;
    for (uint8_t depth = 0; depth < 8; depth++) {
        if (!cur || !xi_copy_is_identity_alias(cur) || cur->nargs < 1)
            return cur;
        cur = cur->args[0];
    }
    return cur;
}

static bool xi_task_loop_const_int_value(const XiValue *v, int64_t expected) {
    v = xi_task_loop_unwrap_identity(v);
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_INT &&
           v->aux_int == expected;
}

static bool xi_task_loop_is_add_one_from_phi(const XiValue *v, const XiValue *phi) {
    v = xi_task_loop_unwrap_identity(v);
    if (!v || !phi || v->op != XI_ADD || v->nargs < 2)
        return false;
    const XiValue *lhs = xi_task_loop_unwrap_identity(v->args[0]);
    const XiValue *rhs = xi_task_loop_unwrap_identity(v->args[1]);
    return (lhs == phi && xi_task_loop_const_int_value(rhs, 1)) ||
           (rhs == phi && xi_task_loop_const_int_value(lhs, 1));
}

static bool xi_task_loop_loads_array_length(XiFunc *f, const XiValue *v, XiValue *arr) {
    v = xi_task_loop_unwrap_identity(v);
    if (!v || v->op != XI_LEN || v->nargs != 1)
        return false;
    return xi_task_array_values_same(f, (XiValue *) v->args[0], arr);
}

static bool xi_task_loop_header_checks_index_below_length(XiFunc *f, const XiBlock *header,
                                                          const XiValue *index, XiValue *arr) {
    if (!header || header->kind != XI_BLOCK_IF || !header->control)
        return false;
    const XiValue *control = xi_task_loop_unwrap_identity(header->control);
    if (!control || control->nargs < 2)
        return false;
    const XiValue *lhs = xi_task_loop_unwrap_identity(control->args[0]);
    const XiValue *rhs = xi_task_loop_unwrap_identity(control->args[1]);
    switch ((XiOp) control->op) {
        case XI_LT:
            return lhs == index && xi_task_loop_loads_array_length(f, rhs, arr);
        case XI_GT:
            return rhs == index && xi_task_loop_loads_array_length(f, lhs, arr);
        default:
            return false;
    }
}

static bool xi_task_loop_find_counted_latch(const XiValue *index, const XiBlock *header,
                                            const XiBlock **out_latch) {
    if (out_latch)
        *out_latch = NULL;
    index = xi_task_loop_unwrap_identity(index);
    if (!index || !header || index->op != XI_PHI || index->block != header ||
        index->nargs != header->npreds)
        return false;

    bool has_zero_base = false;
    const XiBlock *latch = NULL;
    for (uint16_t i = 0; i < index->nargs; i++) {
        const XiValue *arg = xi_task_loop_unwrap_identity(index->args[i]);
        if (xi_task_loop_const_int_value(arg, 0)) {
            has_zero_base = true;
            continue;
        }
        if (!xi_task_loop_is_add_one_from_phi(arg, index))
            return false;
        if (latch && latch != header->preds[i])
            return false;
        latch = header->preds[i];
    }
    if (!has_zero_base || !latch)
        return false;
    if (out_latch)
        *out_latch = latch;
    return true;
}

static bool xi_task_loop_body_reaches_latch_directly(const XiBlock *body, const XiBlock *latch) {
    if (!body || !latch)
        return false;
    return body == latch || body->succs[0] == latch || body->succs[1] == latch;
}

static bool xi_task_index_get_is_counted_consuming_loop(XiFunc *f, XiValue *index_get,
                                                        XiValue *arr) {
    if (!f || !index_get || index_get->op != XI_INDEX_GET || index_get->nargs < 2 ||
        !xi_task_array_values_same(f, index_get->args[0], arr))
        return false;
    if (!xi_plain_await_for_task_index_get(f, index_get))
        return false;

    const XiValue *index = xi_task_loop_unwrap_identity(index_get->args[1]);
    if (!index || index->op != XI_PHI || !index->block)
        return false;
    const XiBlock *header = index->block;
    const XiBlock *body = index_get->block;
    const XiBlock *latch = NULL;
    if (!xi_task_loop_find_counted_latch(index, header, &latch))
        return false;
    if (!xi_task_loop_header_checks_index_below_length(f, header, index, arr))
        return false;
    if (header->succs[0] != body)
        return false;
    return xi_task_loop_body_reaches_latch_directly(body, latch);
}

static bool xi_task_array_single_plain_await_index_get(XiFunc *f, XiValue *arr,
                                                       XiValue **out_index_get,
                                                       XiValue **out_await) {
    if (out_index_get)
        *out_index_get = NULL;
    if (out_await)
        *out_await = NULL;
    uint32_t count = 0;
    XiValue *only_index_get = NULL;
    XiValue *only_await = NULL;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_INDEX_GET || v->nargs < 1 ||
                !xi_task_array_values_same(f, v->args[0], arr))
                continue;
            XiValue *await = xi_plain_await_for_task_index_get(f, v);
            if (!await)
                return false;
            count++;
            only_index_get = v;
            only_await = await;
            if (count > 1)
                return false;
        }
    }
    if (count != 1)
        return false;
    if (out_index_get)
        *out_index_get = only_index_get;
    if (out_await)
        *out_await = only_await;
    return true;
}

static bool xi_task_array_allowed_sequential_await_use(XiFunc *f, XiValue *user, uint16_t arg_idx,
                                                       XiValue *arr, bool *saw_await) {
    if (!user || !arr || arg_idx != 0)
        return false;
    /* Ownership is explicit before optimization in the staged pipeline. ARC
     * bookkeeping does not make the task array semantically observable and
     * therefore must not defeat the consuming-loop proof. */
    if (xi_task_array_use_is_ownership_bookkeeping(user, arg_idx))
        return true;
    if (xi_await_all_task_array_allowed_structural_use(f, user, arg_idx, arr))
        return true;
    if (user->op == XI_LEN && xi_task_array_values_same(f, user->args[0], arr))
        return true;
    if (user->op == XI_INDEX_GET && xi_task_array_values_same(f, user->args[0], arr) &&
        xi_plain_await_consumes_task_index_get(f, user)) {
        if (saw_await)
            *saw_await = true;
        return true;
    }
    return false;
}

static bool xi_task_array_can_defer_batch_for_sequential_awaits(XiFunc *f, XiValue *arr) {
    if (!f || !xi_task_array_root_is_known(f, arr))
        return false;

    bool saw_set = false;
    bool saw_await = false;
    bool saw_clear = false;
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v)
                continue;
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (!xi_task_array_values_same(f, v->args[a], arr))
                    continue;
                if (xi_task_array_use_is_shared_init(f, v, a, arr))
                    continue;
                if (a == 0 && xi_await_all_task_array_pushes_go(f, v, arr)) {
                    saw_set = true;
                    continue;
                }
                if (xi_task_array_use_is_clear(f, v, a, arr))
                    saw_clear = true;
                if (xi_task_array_allowed_sequential_await_use(f, v, a, arr, &saw_await))
                    continue;
                return false;
            }
        }
    }
    return saw_set && saw_await && saw_clear;
}

static bool xi_mark_task_array_deferred_batch_go_producers(XiFunc *f, XiValue *arr,
                                                           XiPassChange *chg) {
    if (!xi_task_array_can_defer_batch_for_sequential_awaits(f, arr))
        return false;

    bool changed = false;
    XiValue *one_shot_index_get = NULL;
    XiValue *one_shot_await = NULL;
    bool can_one_shot_consuming_loop =
        xi_task_array_single_plain_await_index_get(f, arr, &one_shot_index_get, &one_shot_await) &&
        xi_task_index_get_is_counted_consuming_loop(f, one_shot_index_get, arr);
    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->nargs < 2)
                continue;
            XiValue *go = xi_await_all_task_array_pushed_go(f, v, arr);
            if (go) {
                if ((go->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0) {
                    go->aux_int |= XI_GO_AUX_ONE_SHOT_AWAIT;
                    changed = true;
                }
                if ((go->aux_int & XI_GO_AUX_DEFER_BATCH) == 0) {
                    go->aux_int |= XI_GO_AUX_DEFER_BATCH;
                    changed = true;
                }
            }
            if (v->op == XI_INDEX_GET && v->nargs >= 1 &&
                xi_task_array_values_same(f, v->args[0], arr)) {
                XiValue *await = xi_plain_await_for_task_index_get(f, v);
                if (await && (await->aux_int & XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH) == 0) {
                    await->aux_int |= XI_AWAIT_AUX_SUBMIT_DEFERRED_BATCH;
                    changed = true;
                }
                if (await && can_one_shot_consuming_loop && v == one_shot_index_get &&
                    await == one_shot_await && (await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0) {
                    await->aux_int |= XI_AWAIT_AUX_CONSUME_TASK;
                    changed = true;
                }
            }
        }
    }

    if (changed && chg) {
        chg->values_changed = true;
        chg->n_added++;
    }
    return changed;
}

XR_FUNC XiPassChange xi_opt_mark_one_shot_await(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_mark_one_shot_await: NULL func");
    XiPassChange chg = xi_pass_no_change();

    compute_use_counts(f);

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!v || v->op != XI_GO)
                continue;
            if (xi_mark_fire_and_forget_result_group_go_deferred(f, v, &chg))
                continue;
            if (!xi_go_can_be_one_shot_awaited(v))
                continue;

            XiValue *await = find_one_shot_await_for_go(f, v);
            if (!await)
                continue;

            bool changed = false;
            if ((await->aux_int & XI_AWAIT_AUX_CONSUME_TASK) == 0) {
                await->aux_int |= XI_AWAIT_AUX_CONSUME_TASK;
                changed = true;
            }
            if ((v->aux_int & XI_GO_AUX_ONE_SHOT_AWAIT) == 0) {
                v->aux_int |= XI_GO_AUX_ONE_SHOT_AWAIT;
                changed = true;
            }
            if (changed) {
                chg.values_changed = true;
                chg.n_added++;
            }
        }
    }

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (!xi_await_is_all_tasks(v))
                continue;
            xi_mark_await_all_fresh_task_array_one_shot(f, v, v->args[0], &chg);
        }
    }

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v && (v->op == XI_ARRAY_NEW || v->op == XI_GET_SHARED))
                xi_mark_task_array_deferred_batch_go_producers(f, v, &chg);
        }
    }

    return chg;
}

XR_FUNC XiPassChange xi_opt_dce(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_dce: NULL func");
    XiPassChange chg = xi_pass_no_change();

    compute_use_counts(f);

    /* Iteratively remove dead values (values with 0 uses and no side effects).
     * Removing a value may make its operands dead, so we iterate. */
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = 0; b < f->nblocks; b++) {
            XiBlock *blk = f->blocks[b];

            for (uint32_t i = 0; i < blk->nvalues; /* no increment */) {
                XiValue *v = blk->values[i];

                /* Keep if: has uses, has side effects, or may throw */
                if (v->uses > 0 || (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW))) {
                    i++;
                    continue;
                }

                /* Dead value: decrement use counts of operands */
                for (uint16_t a = 0; a < v->nargs; a++) {
                    if (v->args[a])
                        v->args[a]->uses--;
                }

                block_remove_value(blk, i);
                changed = true;
                chg.values_changed = true;
                chg.n_removed++;
                /* Don't increment i: next value shifted into position */
            }
        }
    }
    return chg;
}

/* ========== Phi Simplification ========== */

XR_FUNC XiPassChange xi_opt_phi_simplify(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_phi_simplify: NULL func");
    XiPassChange chg = xi_pass_no_change();

    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t b = 0; b < f->nblocks; b++) {
            XiBlock *blk = f->blocks[b];
            XiPhi **prev_ptr = &blk->phis;
            XiPhi *phi = blk->phis;

            while (phi) {
                XiPhi *next = phi->next;

                /* Find the unique non-self operand */
                XiValue *unique = NULL;
                bool trivial = true;

                for (uint16_t a = 0; a < phi->value.nargs; a++) {
                    XiValue *arg = phi->value.args[a];
                    if (!arg || arg == &phi->value)
                        continue; /* skip self-references and NULLs */
                    if (unique == NULL) {
                        unique = arg;
                    } else if (arg != unique) {
                        trivial = false;
                        break;
                    }
                }

                if (trivial && unique) {
                    /* Replace all uses of this phi with the unique operand */
                    replace_all_uses(f, &phi->value, unique);
                    /* Remove phi from linked list */
                    *prev_ptr = next;
                    changed = true;
                    chg.values_changed = true;
                    chg.n_removed++;
                } else {
                    prev_ptr = &phi->next;
                }

                phi = next;
            }
        }
    }
    return chg;
}

/* Strength reduction lives in xi_opt_strength.c (dedicated). */

/* ========== SelectRepresentations ========== */

/* Determine the machine representation a value naturally produces.
 * Constants and arithmetic with known numeric types produce I64/F64.
 * Calls, loads, and polymorphic ops produce TAGGED. */
static XrRep sr_type_scalar_rep(const struct XrType *type) {
    if (!type)
        return XR_REP_TAGGED;
    if (type->is_nullable || type->kind == XR_KIND_NULL ||
        xr_type_intrinsically_includes_null(type))
        return XR_REP_TAGGED;
    if (type->kind == XR_KIND_POINTER)
        return XR_REP_RAWPTR;
    /* CFn<...> is a first-class bare C function pointer (no closure header),
     * represented like a raw pointer address. */
    if (XR_TYPE_IS_C_FUNCTION(type))
        return XR_REP_RAWPTR;
    XrRep r = xr_type_rep(type);
    return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
}

static XrRep sr_type_native_boundary_rep(const struct XrType *type) {
    if (!type)
        return XR_REP_TAGGED;
    XrRep scalar = sr_type_scalar_rep(type);
    if (scalar != XR_REP_TAGGED)
        return scalar;
    if (type->is_nullable)
        return XR_REP_TAGGED;
    switch (type->kind) {
        case XR_KIND_POINTER:
            return XR_REP_RAWPTR;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_TUPLE:
            return XR_REP_PTR;
        default:
            return XR_REP_TAGGED;
    }
}

/* The return boundary of a String-returning function. String has a single
 * storage fact: the tagged outer value. A native String return would need a
 * representation adapter no frozen storage row can state, so the boundary
 * stays tagged and the callee ABI conversion is left to the emitter. Both the
 * rewrite and the adapter predicate ask this one question so they cannot
 * disagree on which returns carry an adapter. */
static XrRep sr_type_return_boundary_rep(const struct XrType *type) {
    if (type && type->kind == XR_KIND_STRING && !type->is_nullable)
        return XR_REP_TAGGED;
    return sr_type_native_boundary_rep(type);
}

/* A call-bound place must use the same pointee representation as its ABI
 * slot.  Heap/reference aggregates stay tagged so taking their address does
 * not reinterpret a native object pointer as an XrValue (or vice versa).
 * Scalar, raw-pointer, fixed-array and Slice lanes retain their native view. */
static XrRep sr_type_call_place_pointee_rep(const struct XrType *type) {
    if (!type)
        return XR_REP_TAGGED;
    XrRep scalar = sr_type_scalar_rep(type);
    if (scalar != XR_REP_TAGGED)
        return scalar;
    if (type->is_nullable)
        return XR_REP_TAGGED;
    switch (type->kind) {
        case XR_KIND_FIXED_ARRAY:
        case XR_KIND_SLICE:
            return XR_REP_PTR;
        default:
            return XR_REP_TAGGED;
    }
}

static bool sr_convert_can_return_null(const XiValue *v) {
    if (!v || v->op != XI_CONVERT || v->nargs < 1 || !v->type)
        return false;
    if (v->type->kind != XR_KIND_INT && v->type->kind != XR_KIND_FLOAT)
        return false;

    const XrType *src = v->args[0] ? v->args[0]->type : NULL;
    if (!src || src->is_nullable)
        return true;

    switch (src->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
            return false;
        case XR_KIND_POINTER:
            return v->type->kind != XR_KIND_INT;
        default:
            return true;
    }
}

static bool sr_as_is_native_numeric_width(const XiValue *v) {
    if (!v || v->op != XI_AS || (v->aux_int & 1) != 0 || (int32_t) (v->aux_int >> 1) >= 0 ||
        v->nargs < 1 || !v->args[0] || !v->type || !v->args[0]->type)
        return false;
    bool same_numeric_domain = (XR_TYPE_IS_INT(v->type) && XR_TYPE_IS_INT(v->args[0]->type)) ||
                               (XR_TYPE_IS_FLOAT(v->type) && XR_TYPE_IS_FLOAT(v->args[0]->type));
    return same_numeric_domain && sr_type_scalar_rep(v->type) != XR_REP_TAGGED &&
           sr_type_scalar_rep(v->args[0]->type) != XR_REP_TAGGED;
}

static bool sr_type_is_task_instance(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return xr_type_is_builtin_named_class(type, "Task");
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (sr_type_is_task_instance(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool sr_type_is_channel(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_CHANNEL)
        return true;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (sr_type_is_channel(type->union_type.members[i]))
                return true;
        }
    }
    return false;
}

static bool sr_is_channel_recv_method(const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1 || !sr_type_is_channel(v->args[0]->type))
        return false;
    const char *method = (const char *) v->aux;
    return method && (strcmp(method, "recv") == 0 || strcmp(method, "tryRecv") == 0 ||
                      strcmp(method, "recvOr") == 0);
}

static bool sr_field_receiver_uses_native_rep(const XiValue *receiver) {
    if (!receiver || !receiver->type || receiver->type->kind != XR_KIND_INSTANCE)
        return false;
    return !sr_type_is_task_instance(receiver->type);
}

static const XrType *sr_array_elem_type(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ARRAY || type->kind == XR_KIND_SLICE)
        return type->container.element_type;
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return type->fixed_array.element_type;
    return NULL;
}

static XrRep sr_typed_array_elem_rep(const XrType *type) {
    const XrType *elem = sr_array_elem_type(type);
    if (!elem || elem->is_nullable)
        return XR_REP_TAGGED;
    switch (elem->scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
            return XR_REP_I64;
        case XR_NATIVE_F32:
            return XR_REP_F64;
        default:
            return sr_type_scalar_rep(elem);
    }
}

static bool sr_type_has_static_typed_array_storage(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    if (type->kind != XR_KIND_ARRAY && type->kind != XR_KIND_SLICE && type->kind != XR_KIND_SLICE)
        return false;
    return sr_typed_array_elem_rep(type) != XR_REP_TAGGED;
}

static const XiValue *sr_unwrap_identity_value(const XiValue *v) {
    while (v &&
           (v->op == XI_BOX || v->op == XI_UNBOX || xi_copy_is_identity_alias(v) ||
            xi_op_is_identity_forward(v->op)) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

static XrRep sr_value_typed_array_elem_rep_depth(const XiValue *value, uint8_t depth) {
    const XiValue *v = sr_unwrap_identity_value(value);
    if (!v || depth > 8)
        return XR_REP_TAGGED;

    XrRep own = sr_typed_array_elem_rep(v->type);
    if (own != XR_REP_TAGGED)
        return own;

    if (v->op == XI_SLICE && v->nargs >= 1)
        return sr_value_typed_array_elem_rep_depth(v->args[0], (uint8_t) (depth + 1));

    if (v->op == XI_PHI) {
        bool has_base = false;
        XrRep first = XR_REP_TAGGED;
        if (v->nargs == 0)
            return XR_REP_TAGGED;
        for (uint16_t i = 0; i < v->nargs; i++) {
            const XiValue *arg = sr_unwrap_identity_value(v->args[i]);
            if (arg == v)
                continue;
            XrRep arg_rep = sr_value_typed_array_elem_rep_depth(arg, (uint8_t) (depth + 1));
            if (arg_rep == XR_REP_TAGGED)
                return XR_REP_TAGGED;
            if (!has_base) {
                first = arg_rep;
                has_base = true;
            } else if (first != arg_rep) {
                return XR_REP_TAGGED;
            }
        }
        return has_base ? first : XR_REP_TAGGED;
    }

    return XR_REP_TAGGED;
}

static XrRep sr_value_typed_array_elem_rep(const XiValue *value) {
    return sr_value_typed_array_elem_rep_depth(value, 0);
}

static bool sr_value_has_static_typed_array_storage_depth(const XiValue *value, uint8_t depth) {
    const XiValue *v = sr_unwrap_identity_value(value);
    if (!v || depth > 8)
        return false;

    /* Array<T>/Slice<T> with a native element type is a language-level typed
     * storage invariant.  The value may come from a param, local constructor,
     * method result, import, or direct call.  AOT prepare/cgen still decides
     * whether a particular access can use raw storage; select_rep only keeps
     * the element value itself in its native register representation. */
    if (sr_type_has_static_typed_array_storage(v->type))
        return true;
    if (v->op == XI_SLICE && v->nargs >= 1)
        return sr_value_has_static_typed_array_storage_depth(v->args[0], (uint8_t) (depth + 1));
    if (v->op == XI_PHI) {
        bool has_base = false;
        if (v->nargs == 0)
            return false;
        for (uint16_t i = 0; i < v->nargs; i++) {
            const XiValue *arg = sr_unwrap_identity_value(v->args[i]);
            if (arg == v)
                continue;
            if (!sr_value_has_static_typed_array_storage_depth(arg, (uint8_t) (depth + 1)))
                return false;
            has_base = true;
        }
        return has_base;
    }
    return false;
}

static bool sr_value_has_static_typed_array_storage(const XiValue *value) {
    return sr_value_has_static_typed_array_storage_depth(value, 0);
}

static bool sr_value_is_fixed_array_field_ref(const XiValue *value) {
    const XiValue *v = sr_unwrap_identity_value(value);
    if (!v || v->op != XI_AGG_GET || v->nargs < 1)
        return false;
    const XrAggregateLayout *sl = (const XrAggregateLayout *) v->aux;
    if (!sl || v->aux_int < 0 || v->aux_int >= sl->field_count)
        return false;
    return sl->fields[v->aux_int].native_type == XR_NATIVE_ARRAY;
}

static bool sr_value_is_typed_array_field_ref(const XiValue *value) {
    const XiValue *v = sr_unwrap_identity_value(value);
    if (!v || v->op != XI_AGG_GET || v->nargs < 1)
        return false;
    const XrAggregateLayout *sl = (const XrAggregateLayout *) v->aux;
    if (!sl || v->aux_int < 0 || v->aux_int >= sl->field_count)
        return false;
    return sl->fields[v->aux_int].native_type == XR_NATIVE_ARRAY_REF &&
           sr_typed_array_elem_rep(v->type) != XR_REP_TAGGED;
}

static bool sr_value_has_static_index_storage(const XiValue *value) {
    const XiValue *v = sr_unwrap_identity_value(value);
    bool uniform_array_container =
        v && v->type && !v->type->is_nullable &&
        (v->type->kind == XR_KIND_ARRAY || v->type->kind == XR_KIND_SLICE);
    return uniform_array_container || sr_value_has_static_typed_array_storage(value) ||
           sr_value_is_typed_array_field_ref(value) || sr_value_is_fixed_array_field_ref(value);
}

static bool sr_value_has_static_unboxed_array_elem_type(const XiValue *value) {
    return sr_value_typed_array_elem_rep(value) != XR_REP_TAGGED;
}

/* The container operand of an index access. A freshly allocated heap array has
 * a single storage fact, the owned tagged outer value, so a native pointer view
 * of it would need a representation adapter no frozen storage row can state.
 * Its container operand stays tagged; the index and the element keep their
 * native representations. Every other container keeps the native boundary it
 * already carries at its own definition, so no adapter appears there either. */
static XrRep sr_container_operand_rep(const XiValue *container) {
    const XiValue *v = sr_unwrap_identity_value(container);
    if (v && v->op == XI_ARRAY_NEW)
        return XR_REP_TAGGED;
    return sr_type_native_boundary_rep(container ? container->type : NULL);
}

static bool sr_is_typed_array_length_field(const XiValue *v) {
    if (!v || v->op != XI_LEN || v->nargs != 1 || !v->args[0])
        return false;
    if (!sr_value_has_static_typed_array_storage(v->args[0]) &&
        !sr_value_is_typed_array_field_ref(v->args[0]))
        return false;
    return true;
}

static bool sr_is_static_collection_length_field(const XiValue *v) {
    if (!v || v->op != XI_LEN || v->nargs != 1 || !v->args[0])
        return false;
    const XiValue *receiver = sr_unwrap_identity_value(v->args[0]);
    if (!receiver || !receiver->type)
        return false;
    return receiver->type->kind == XR_KIND_ARRAY || receiver->type->kind == XR_KIND_SLICE ||
           receiver->type->kind == XR_KIND_SLICE || receiver->type->kind == XR_KIND_MAP ||
           receiver->type->kind == XR_KIND_SET;
}

static bool sr_type_is_native_map_key(const XrType *type) {
    return type && !type->is_nullable &&
           (type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT || type->kind == XR_KIND_BOOL);
}

static bool sr_type_is_native_map_value(const XrType *type) {
    return sr_type_is_native_map_key(type);
}

static bool sr_type_native_map_value_rep(const XrType *type, XrRep *out_rep) {
    if (!type || type->kind != XR_KIND_MAP || !sr_type_is_native_map_key(type->map.key_type) ||
        !sr_type_is_native_map_value(type->map.value_type))
        return false;
    XrRep key_rep = sr_type_scalar_rep(type->map.key_type);
    XrRep value_rep = sr_type_scalar_rep(type->map.value_type);
    if ((key_rep != XR_REP_I64 && key_rep != XR_REP_F64) ||
        (value_rep != XR_REP_I64 && value_rep != XR_REP_F64))
        return false;
    if (out_rep)
        *out_rep = value_rep;
    return true;
}

static bool sr_method_name_is(const XiValue *v, const char *name) {
    const char *method = v && v->aux ? (const char *) v->aux : NULL;
    if (method && strcmp(method, name) == 0)
        return true;
    if (!v || !name || v->aux_int < 0)
        return false;

    SymbolId symbol = (SymbolId) (v->aux_int >> 1);
    if (strcmp(name, "containsKey") == 0)
        return symbol == SYMBOL_CONTAINS_KEY;
    if (strcmp(name, "get") == 0)
        return symbol == SYMBOL_GET;
    return false;
}

static bool sr_type_is_pod_span_elem(const XrType *type) {
    if (!type || type->is_nullable)
        return false;
    switch (type->kind) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        default:
            return false;
    }
}

static bool sr_builtin_receiver_registry_matches(const XrType *receiver_type,
                                                 XaBuiltinReceiverKind kind) {
    switch (kind) {
        case XA_BUILTIN_RECEIVER_EXACT_INTEGER:
            return receiver_type && receiver_type->kind == XR_KIND_INT &&
                   !receiver_type->is_nullable;
        case XA_BUILTIN_RECEIVER_EXACT_UNSIGNED_INTEGER:
            return xr_type_is_exact_unsigned_integer(receiver_type);
        case XA_BUILTIN_RECEIVER_U8_ARRAY:
            return xr_type_is_u8_array(receiver_type);
        case XA_BUILTIN_RECEIVER_ARRAY:
            return receiver_type && receiver_type->kind == XR_KIND_ARRAY;
        case XA_BUILTIN_RECEIVER_U8_SLICE:
            return xr_type_is_u8_slice(receiver_type);
        case XA_BUILTIN_RECEIVER_POD_SLICE:
            return receiver_type && receiver_type->kind == XR_KIND_SLICE &&
                   sr_type_is_pod_span_elem(receiver_type->container.element_type);
    }
    return false;
}

static bool sr_call_method_matches_receiver_registry_id(const XiValue *v,
                                                        XaBuiltinReceiverMethodId method_id) {
    const XaBuiltinReceiverMethodSpec *spec = xa_builtin_receiver_method_by_id(method_id);
    if (!spec || !v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) ||
        v->nargs < 1 || !v->args[0] || !v->aux)
        return false;
    return strcmp((const char *) v->aux, spec->source_name) == 0 &&
           sr_builtin_receiver_registry_matches(v->args[0]->type, spec->receiver);
}

static bool sr_is_typed_array_native_receiver_method(const XiValue *v) {
    if (!v || (v->op != XI_CALL_METHOD && v->op != XI_CALL_METHOD_DIRECT) || v->nargs < 1 ||
        !v->args[0] || !sr_value_has_static_typed_array_storage(v->args[0]))
        return false;
    return sr_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_PUSH) ||
           sr_call_method_matches_receiver_registry_id(v,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESERVE) ||
           sr_call_method_matches_receiver_registry_id(v,
                                                       XA_BUILTIN_RECEIVER_METHOD_ARRAY_RESIZE) ||
           sr_call_method_matches_receiver_registry_id(
               v, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_APPEND_FROM) ||
           sr_call_method_matches_receiver_registry_id(
               v, XA_BUILTIN_RECEIVER_METHOD_U8_ARRAY_REPEAT_FROM) ||
           sr_call_method_matches_receiver_registry_id(v, XA_BUILTIN_RECEIVER_METHOD_ARRAY_FILL);
}

static bool sr_type_is_named_instance(const XrType *type, const char *name) {
    if (!type || !name)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, name) == 0;
    if (type->kind == XR_KIND_UNION) {
        for (uint8_t i = 0; i < type->union_type.member_count; i++) {
            if (sr_type_is_named_instance(type->union_type.members[i], name))
                return true;
        }
    }
    return false;
}

static const XrType *sr_atomic_inner_type(const XiValue *recv) {
    const XiValue *origin = sr_unwrap_identity_value(recv);
    const XrType *type = origin ? origin->type : (recv ? recv->type : NULL);
    if (!sr_type_is_named_instance(type, "Atomic"))
        return NULL;
    if (type->kind != XR_KIND_INSTANCE || type->instance.type_arg_count == 0 ||
        !type->instance.type_args)
        return NULL;
    return type->instance.type_args[0];
}

static bool sr_atomic_method_return_rep(const XiValue *v, XrRep *out_rep) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs < 1)
        return false;
    const XrType *inner = sr_atomic_inner_type(v->args[0]);
    if (!inner)
        return false;
    if (sr_method_name_is(v, "load") || sr_method_name_is(v, "swap") ||
        sr_method_name_is(v, "fetchAdd") || sr_method_name_is(v, "fetchSub")) {
        XrRep rep = sr_type_scalar_rep(inner);
        if (inner->kind != XR_KIND_BOOL && (rep == XR_REP_I64 || rep == XR_REP_F64)) {
            if (out_rep)
                *out_rep = rep;
            return true;
        }
    }
    return false;
}

static bool sr_same_value_shape(const XiValue *a, const XiValue *b, uint8_t depth) {
    a = sr_unwrap_identity_value(a);
    b = sr_unwrap_identity_value(b);
    if (a == b)
        return true;
    if (!a || !b || depth > 8 || a->op != b->op)
        return false;

    switch (a->op) {
        case XI_CONST:
            if (!a->type || !b->type || a->type->kind != b->type->kind)
                return false;
            if (a->type->kind == XR_KIND_STRING) {
                const char *as = (const char *) a->aux;
                const char *bs = (const char *) b->aux;
                return as && bs && strcmp(as, bs) == 0;
            }
            return a->aux_int == b->aux_int;
        case XI_PARAM:
        case XI_GET_SHARED:
        case XI_GET_GLOBAL:
        case XI_LOAD_UPVAL:
            return a->aux_int == b->aux_int;
        case XI_LOAD_FIELD:
            if (a->aux && b->aux) {
                if (strcmp((const char *) a->aux, (const char *) b->aux) != 0)
                    return false;
            } else if (a->aux_int != b->aux_int) {
                return false;
            }
            return a->nargs >= 1 && b->nargs >= 1 &&
                   sr_same_value_shape(a->args[0], b->args[0], (uint8_t) (depth + 1));
        case XI_AGG_GET:
        case XI_TUPLE_GET:
            return a->aux == b->aux && a->aux_int == b->aux_int && a->nargs >= 1 && b->nargs >= 1 &&
                   sr_same_value_shape(a->args[0], b->args[0], (uint8_t) (depth + 1));
        case XI_CONVERT:
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
        case XI_NARROW_F32:
        case XI_WIDEN_F32:
            return a->conversion.kind == b->conversion.kind &&
                   a->conversion.source_scalar_rep == b->conversion.source_scalar_rep &&
                   a->conversion.target_scalar_rep == b->conversion.target_scalar_rep &&
                   a->conversion.is_implicit == b->conversion.is_implicit &&
                   a->conversion.is_compile_time == b->conversion.is_compile_time &&
                   a->nargs >= 1 && b->nargs >= 1 &&
                   sr_same_value_shape(a->args[0], b->args[0], (uint8_t) (depth + 1));
        default:
            return false;
    }
}

static bool sr_value_invalidates_map_guard(const XiValue *v) {
    if (!v || v->op == XI_ERR_CHECK)
        return false;
    return (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW |
                        XI_FLAG_MAY_SUSPEND)) != 0;
}

static bool sr_block_has_no_guard_invalidating_effect_before(const XiBlock *blk,
                                                             const XiValue *target) {
    if (!blk || !target)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (v == target)
            return true;
        if (sr_value_invalidates_map_guard(v))
            return false;
    }
    return false;
}

static bool sr_block_has_no_guard_invalidating_effect_after(const XiBlock *blk,
                                                            const XiValue *start) {
    bool seen = start == NULL;
    bool found = false;
    if (!blk)
        return false;
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        const XiValue *v = blk->values[i];
        if (!v)
            continue;
        if (v == start) {
            seen = true;
            found = true;
            continue;
        }
        if (seen && sr_value_invalidates_map_guard(v))
            return false;
    }
    if (!found && start != NULL) {
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            const XiValue *v = blk->values[i];
            if (sr_value_invalidates_map_guard(v))
                return false;
        }
        return true;
    }
    return seen;
}

static bool sr_map_has_control_matches_get(const XiValue *control, const XiValue *get) {
    const XiValue *has = sr_unwrap_identity_value(control);
    if (!has || has->op != XI_CALL_METHOD || has->nargs != 2 ||
        !sr_method_name_is(has, "containsKey"))
        return false;
    if (!get || get->op != XI_CALL_METHOD || get->nargs != 2 || !sr_method_name_is(get, "get"))
        return false;
    return sr_same_value_shape(has->args[0], get->args[0], 0) &&
           sr_same_value_shape(has->args[1], get->args[1], 0);
}

static bool sr_map_get_has_present_guard(const XiValue *v) {
    if (!v || v->op != XI_CALL_METHOD || v->nargs != 2 || !sr_method_name_is(v, "get") ||
        !v->block || !v->args[0])
        return false;
    if (!sr_type_native_map_value_rep(v->args[0]->type, NULL))
        return false;
    if (!sr_block_has_no_guard_invalidating_effect_before(v->block, v))
        return false;
    if (v->block->npreds == 0)
        return false;
    for (uint16_t i = 0; i < v->block->npreds; i++) {
        const XiBlock *pred = v->block->preds[i];
        if (!pred || pred->kind != XI_BLOCK_IF || pred->succs[0] != v->block)
            return false;
        if (!sr_map_has_control_matches_get(pred->control, v))
            return false;
        if (!sr_block_has_no_guard_invalidating_effect_after(pred, pred->control))
            return false;
    }
    return true;
}

static XrRep sr_def_rep(const XiValue *v, const XiRepPolicy *policy);

static bool sr_param_uses_default_sentinel(const XiValue *v) {
    if (!v || v->op != XI_PARAM || !v->block || !v->block->func)
        return false;
    const XiFunc *f = v->block->func;
    if (f->entry_type != 1)
        return false;
    if (v->aux_int < 0)
        return false;
    uint64_t param_index = (uint64_t) v->aux_int;
    return param_index >= f->min_params && param_index < f->nparams;
}

static bool sr_param_is_call_bound_place(const XiValue *v) {
    if (!v || v->op != XI_PARAM || !v->block || !v->block->func || v->aux_int < 0 ||
        v->aux_int > UINT16_MAX)
        return false;
    XrParamMode mode = xi_func_param_passing_mode(v->block->func, (uint16_t) v->aux_int);
    if (mode == XR_PARAM_REF)
        return true;
    if (xi_value_is_read_place_param(v))
        return true;
    const XiFunc *func = v->block->func;
    return mode == XR_PARAM_READ && v->aux_int == 0 && func->receiver_call_place && func->params &&
           func->params[0] == v;
}

static bool sr_value_is_null_const(const XiValue *v) {
    return v && v->op == XI_CONST && v->type && v->type->kind == XR_KIND_NULL;
}

static bool sr_compare_uses_null(const XiValue *user) {
    return user && user->nargs >= 2 &&
           (sr_value_is_null_const(user->args[0]) || sr_value_is_null_const(user->args[1]));
}

static bool sr_select_value_arms_need_tagged(const XiValue *v, const XiRepPolicy *policy) {
    if (!v || v->op != XI_SELECT || v->nargs < 3)
        return false;
    return sr_def_rep(v->args[1], policy) == XR_REP_TAGGED ||
           sr_def_rep(v->args[2], policy) == XR_REP_TAGGED;
}

static bool sr_def_rep_memory_op(const XiValue *v, XrRep *out) {
    if (!v || !out)
        return false;
    switch (v->op) {
        case XI_INDEX_GET:
            *out = v->nargs >= 1 && v->args[0] && sr_value_has_static_index_storage(v->args[0])
                       ? sr_value_typed_array_elem_rep(v->args[0])
                       : XR_REP_TAGGED;
            return true;
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
            *out = (v->op == XI_BYTE_SLICE_LOAD_F32 || v->op == XI_BYTE_SLICE_LOAD_F64)
                       ? XR_REP_F64
                       : XR_REP_I64;
            return true;
        case XI_SLICE_COMPARE:
        case XI_BYTE_SLICE_COMPARE:
        case XI_BYTE_SLICE_COMMON_PREFIX:
            *out = XR_REP_I64;
            return true;
        case XI_BYTE_SLICE_FILL:
        case XI_BYTE_SLICE_COPY:
        case XI_BYTE_SLICE_REPEAT:
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_FILL:
        case XI_SLICE_COPY:
        case XI_SLICE_REINTERPRET:
            *out = sr_type_native_boundary_rep(v->type);
            return true;
        case XI_ARRAY_DATA_PTR:
            *out = XR_REP_RAWPTR;
            return true;
        case XI_LOCAL_ADDR:
            *out = XR_REP_RAWPTR;
            return true;
        case XI_PLACE_LOAD:
            *out = sr_type_native_boundary_rep(v->type);
            return true;
        case XI_PTR_LOAD:
            *out = sr_type_native_boundary_rep(v->type);
            return true;
        default:
            return false;
    }
}

static bool sr_rep_is_native_number(XrRep rep) {
    return rep == XR_REP_I64 || rep == XR_REP_F64;
}

static XrRep sr_arith_native_result_rep_depth(const XiValue *v, const XiRepPolicy *policy,
                                              uint8_t depth);

static XrRep sr_value_numeric_rep_hint_depth(const XiValue *value, const XiRepPolicy *policy,
                                             uint8_t depth) {
    const XiValue *v = value;
    while (v && (xi_copy_is_identity_alias(v) || xi_op_is_identity_forward(v->op)) && v->nargs >= 1)
        v = v->args[0];
    if (!v || depth > 8 || v->op == XI_BOX)
        return XR_REP_TAGGED;

    XrRep memory_rep = XR_REP_TAGGED;
    if (sr_def_rep_memory_op(v, &memory_rep) && sr_rep_is_native_number(memory_rep))
        return memory_rep;

    XrRep scalar = sr_type_scalar_rep(v->type);
    if (sr_rep_is_native_number(scalar))
        return scalar;

    if (v->op == XI_PHI) {
        if (policy && policy->force_phi_tagged)
            return XR_REP_TAGGED;
        bool has_base = false;
        XrRep first = XR_REP_TAGGED;
        for (uint16_t i = 0; i < v->nargs; i++) {
            const XiValue *arg = v->args[i];
            if (arg == v)
                continue;
            XrRep arg_rep = sr_value_numeric_rep_hint_depth(arg, policy, (uint8_t) (depth + 1));
            if (!sr_rep_is_native_number(arg_rep))
                return XR_REP_TAGGED;
            if (!has_base) {
                first = arg_rep;
                has_base = true;
            } else if (first != arg_rep) {
                return XR_REP_TAGGED;
            }
        }
        return has_base ? first : XR_REP_TAGGED;
    }

    return sr_arith_native_result_rep_depth(v, policy, (uint8_t) (depth + 1));
}

static XrRep sr_arith_native_result_rep_depth(const XiValue *v, const XiRepPolicy *policy,
                                              uint8_t depth) {
    if (!v || depth > 8)
        return XR_REP_TAGGED;

    XrRep type_rep = sr_type_scalar_rep(v->type);
    if (type_rep != XR_REP_TAGGED)
        return type_rep;

    switch (v->op) {
        case XI_NEG:
            return sr_value_numeric_rep_hint_depth(v->args[0], policy, (uint8_t) (depth + 1));
        case XI_BNOT:
        case XI_BIT_BSWAP:
        case XI_BIT_POPCOUNT:
        case XI_BIT_CLZ:
        case XI_BIT_CTZ:
            return sr_value_numeric_rep_hint_depth(v->args[0], policy, (uint8_t) (depth + 1)) ==
                           XR_REP_I64
                       ? XR_REP_I64
                       : XR_REP_TAGGED;
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_SHL:
        case XI_SHR:
        case XI_BIT_ROTL:
        case XI_BIT_ROTR:
        case XI_BIT_MUL_HIGH:
        case XI_MOD: {
            if (v->nargs < 2)
                return XR_REP_TAGGED;
            XrRep lhs = sr_value_numeric_rep_hint_depth(v->args[0], policy, (uint8_t) (depth + 1));
            XrRep rhs = sr_value_numeric_rep_hint_depth(v->args[1], policy, (uint8_t) (depth + 1));
            return lhs == XR_REP_I64 && rhs == XR_REP_I64 ? XR_REP_I64 : XR_REP_TAGGED;
        }
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV: {
            if (v->nargs < 2)
                return XR_REP_TAGGED;
            XrRep lhs = sr_value_numeric_rep_hint_depth(v->args[0], policy, (uint8_t) (depth + 1));
            XrRep rhs = sr_value_numeric_rep_hint_depth(v->args[1], policy, (uint8_t) (depth + 1));
            if (!sr_rep_is_native_number(lhs) || !sr_rep_is_native_number(rhs))
                return XR_REP_TAGGED;
            return lhs == XR_REP_F64 || rhs == XR_REP_F64 ? XR_REP_F64 : XR_REP_I64;
        }
        default:
            return XR_REP_TAGGED;
    }
}

static XrRep sr_arith_native_result_rep(const XiValue *v, const XiRepPolicy *policy) {
    return sr_arith_native_result_rep_depth(v, policy, 0);
}

static bool sr_def_rep_enum_op(const XiValue *value, XrRep *out) {
    switch (value->op) {
        case XI_ENUM_VARIANT_AT:
        case XI_ENUM_PAYLOAD_AT:
        case XI_ENUM_META_GET:
            *out = sr_type_scalar_rep(value->type);
            return true;
        case XI_ENUM_DESCRIPTOR_BOX:
            *out = XR_REP_TAGGED;
            return true;
        case XI_ENUM_DESCRIPTOR_UNBOX:
            *out = XR_REP_I64;
            return true;
        default:
            return false;
    }
}

static bool sr_op_has_arith_native_result(uint16_t op) {
    switch (op) {
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_NEG:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
        case XI_BIT_ROTL:
        case XI_BIT_ROTR:
        case XI_BIT_BSWAP:
        case XI_BIT_POPCOUNT:
        case XI_BIT_CLZ:
        case XI_BIT_MUL_HIGH:
        case XI_BIT_CTZ:
            return true;
        default:
            return false;
    }
}

static XrRep sr_def_rep(const XiValue *v, const XiRepPolicy *policy) {
    if (!v || !v->type)
        return XR_REP_TAGGED;
    XrRep memory_rep = XR_REP_TAGGED;
    if (sr_def_rep_memory_op(v, &memory_rep))
        return memory_rep;
    XrRep enum_rep = XR_REP_TAGGED;
    if (sr_def_rep_enum_op(v, &enum_rep))
        return enum_rep;
    if (sr_op_has_arith_native_result(v->op))
        return sr_arith_native_result_rep(v, policy);
    switch (v->op) {
        case XI_PARAM: {
            if (sr_param_is_call_bound_place(v))
                return XR_REP_RAWPTR;
            if (sr_param_uses_default_sentinel(v))
                return XR_REP_TAGGED;
            /* Typed boundary params get concrete rep.  The AOT backend can
             * use this directly instead of re-inferring from type->kind. */
            return sr_type_native_boundary_rep(v->type);
        }
        case XI_CONST: {
            if (v->type->kind == XR_KIND_NULL || v->type->kind == XR_KIND_STRING)
                return XR_REP_TAGGED;
            return sr_type_scalar_rep(v->type);
        }
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_NOT:
        case XI_ISNULL:
        case XI_IS:
        case XI_CHAN_TRY_SEND:
        case XI_CHAN_IS_CLOSED:
        case XI_CHAN_RECV_STATUS:
            /* bool result (recv_is_value): unboxed i64, never an XrValue.
             * Matches the runtime helper rep and the ISNULL/IS_CLOSED siblings;
             * the select_rep boundary inserts XI_BOX if a tagged use needs it. */
            return XR_REP_I64;
        case XI_SELECT: {
            if (sr_select_value_arms_need_tagged(v, policy))
                return XR_REP_TAGGED;
            return sr_type_scalar_rep(v->type);
        }
        case XI_CHAN_RECV:
        case XI_CHAN_TRY_RECV:
            return XR_REP_TAGGED;
        case XI_CALL:
            /* A String result is stored as the outer tagged value, the same
             * storage a String constant already selects. A native result here
             * would be boxed again at every tagged use, while the frozen
             * storage fact names one tagged owner slot for the call. */
            if (v->type->kind == XR_KIND_STRING && !v->type->is_nullable)
                return XR_REP_TAGGED;
            return sr_type_native_boundary_rep(v->type);
        case XI_CALL_METHOD_DIRECT:
            return sr_type_native_boundary_rep(v->type);
        case XI_ATOMIC_LOAD:
        case XI_ATOMIC_RMW:
            /* Canonical Atomic operations are no longer CALL_METHOD nodes, but
             * they retain the same concrete result contract.  Selecting a
             * tagged default here would re-box fetch/load results after the
             * semantic-intrinsic rewrite and defeat the direct C11 lowering. */
            return sr_type_native_boundary_rep(v->type);
        case XI_CALL_BUILTIN: {
            if (v->xa_intrinsic_id == XA_INTRINSIC_STRING_BYTE_SLICE_VIEW)
                return sr_type_native_boundary_rep(v->type);
            const char *name = (const char *) v->aux;
            if (name && strcmp(name, "array_reserve") == 0)
                return sr_type_native_boundary_rep(v->type);
            return XR_REP_TAGGED;
        }
        case XI_LEN:
            return XR_REP_I64;
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
            return sr_type_native_boundary_rep(v->type);
        case XI_CALL_METHOD: {
            XrRep atomic_rep = XR_REP_TAGGED;
            if (sr_atomic_method_return_rep(v, &atomic_rep))
                return atomic_rep;
        }
            if (sr_is_channel_recv_method(v))
                return XR_REP_TAGGED;
            if (sr_map_get_has_present_guard(v)) {
                XrRep value_rep = XR_REP_TAGGED;
                if (sr_type_native_map_value_rep(v->args[0]->type, &value_rep))
                    return value_rep;
            }
            return sr_type_native_boundary_rep(v->type);
        case XI_LOAD_FIELD:
            if (sr_is_typed_array_length_field(v) || sr_is_static_collection_length_field(v))
                return XR_REP_I64;
            if (policy && policy->prefer_call_args_native && v->nargs >= 1 &&
                sr_field_receiver_uses_native_rep(v->args[0]))
                return sr_type_scalar_rep(v->type);
            return XR_REP_TAGGED;
        case XI_AGG_GET:
            return sr_type_scalar_rep(v->type);
        case XI_PHI:
            if (policy && !policy->force_phi_tagged)
                return sr_type_native_boundary_rep(v->type);
            return XR_REP_TAGGED;
        case XI_BOX:
            return XR_REP_TAGGED;
        case XI_UNBOX: {
            XrRep ur = sr_type_native_boundary_rep(v->type);
            if (ur != XR_REP_TAGGED)
                return ur;
            if (v->nargs >= 1 && v->args[0] && v->args[0]->type) {
                ur = sr_type_native_boundary_rep(v->args[0]->type);
                if (ur != XR_REP_TAGGED)
                    return ur;
            }
            return XR_REP_TAGGED;
        }
        case XI_CONVERT: {
            if (sr_convert_can_return_null(v))
                return XR_REP_TAGGED;
            return sr_type_scalar_rep(v->type);
        }
        case XI_AS:
            /* A numeric `as` normally lowers to NARROW/COPY.  XI_AS survives
             * only when an imported source was still unresolved during
             * per-module lowering.  Once LTO supplies its scalar type, the
             * cast is a native representation-preserving conversion. */
            if (sr_as_is_native_numeric_width(v))
                return sr_type_scalar_rep(v->type);
            return XR_REP_TAGGED;
        /* NARROW/WIDEN: value stays in machine register, only range changes.
         * Integer variants keep I64, float variants keep F64. */
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
            return XR_REP_I64;
        case XI_NARROW_F32:
        case XI_WIDEN_F32:
            return XR_REP_F64;
        default:
            return XR_REP_TAGGED;
    }
}

/*
 * Determine what representation an instruction needs at a given arg position.
 * Arithmetic and comparisons prefer unboxed; everything else wants tagged.
 */
/* Memory-access argument reps (index/bytes/pointer/field/container-new ops),
 * split from sr_use_rep. Returns true when `user` is one of them and writes
 * the rep to *out. */
static bool sr_use_rep_memory_op(const XiValue *user, uint16_t arg_idx, const XiRepPolicy *policy,
                                 XrRep *out) {
    switch (user->op) {
        case XI_INDEX_GET:
            if (user->aux_kind == XI_AUX_KIND_ENUM_CASE) {
                *out = arg_idx == 1 ? XR_REP_I64 : XR_REP_TAGGED;
                return true;
            }
            if (user->nargs >= 2 && user->args[0] &&
                sr_value_has_static_index_storage(user->args[0])) {
                if (arg_idx == 0) {
                    *out = sr_container_operand_rep(user->args[0]);
                    return true;
                }
                if (arg_idx == 1) {
                    *out = XR_REP_I64;
                    return true;
                }
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_INDEX_SET:
            if (user->nargs >= 3 && user->args[0] &&
                (sr_value_has_static_index_storage(user->args[0]) ||
                 sr_value_has_static_unboxed_array_elem_type(user->args[0]))) {
                if (arg_idx == 0) {
                    *out = sr_container_operand_rep(user->args[0]);
                    return true;
                }
                if (arg_idx == 1) {
                    *out = XR_REP_I64;
                    return true;
                }
                if (arg_idx == 2) {
                    *out = sr_value_typed_array_elem_rep(user->args[0]);
                    return true;
                }
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_BYTE_SLICE_LOAD_U16:
        case XI_BYTE_SLICE_LOAD_U32:
        case XI_BYTE_SLICE_LOAD_U64:
        case XI_BYTE_SLICE_LOAD_F32:
        case XI_BYTE_SLICE_LOAD_F64:
            if (arg_idx == 0 && user->nargs >= 1 && user->args[0] &&
                sr_value_has_static_typed_array_storage(user->args[0])) {
                *out = sr_type_native_boundary_rep(user->args[0]->type);
                return true;
            }
            *out = arg_idx == 1 ? XR_REP_I64 : XR_REP_TAGGED;
            return true;
        case XI_BYTE_SLICE_STORE_U16:
        case XI_BYTE_SLICE_STORE_U32:
        case XI_BYTE_SLICE_STORE_U64:
        case XI_BYTE_SLICE_STORE_F32:
        case XI_BYTE_SLICE_STORE_F64:
            if (arg_idx == 0 && user->nargs >= 1 && user->args[0] &&
                sr_value_has_static_typed_array_storage(user->args[0])) {
                *out = sr_type_native_boundary_rep(user->args[0]->type);
                return true;
            }
            if (arg_idx == 1) {
                *out = XR_REP_I64;
            } else if (arg_idx == 2) {
                *out = (user->op == XI_BYTE_SLICE_STORE_F32 || user->op == XI_BYTE_SLICE_STORE_F64)
                           ? XR_REP_F64
                           : XR_REP_I64;
            } else {
                *out = XR_REP_TAGGED;
            }
            return true;
        case XI_BYTE_SLICE_FILL:
        case XI_BYTE_SLICE_REPEAT:
            *out = arg_idx == 0 && user->nargs >= 1 && user->args[0]
                       ? sr_type_native_boundary_rep(user->args[0]->type)
                       : XR_REP_I64;
            return true;
        case XI_SLICE_FILL:
            if (arg_idx == 0 && user->nargs >= 1 && user->args[0]) {
                *out = sr_type_native_boundary_rep(user->args[0]->type);
                return true;
            }
            if (arg_idx == 1 && user->nargs >= 2 && user->args[1]) {
                *out = sr_type_native_boundary_rep(user->args[1]->type);
                return true;
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_SLICE_AS_BYTES:
        case XI_SLICE_REINTERPRET:
            *out = arg_idx == 0 && user->nargs >= 1 && user->args[0]
                       ? sr_type_native_boundary_rep(user->args[0]->type)
                       : XR_REP_TAGGED;
            return true;
        case XI_SLICE_COPY:
        case XI_SLICE_COMPARE:
        case XI_BYTE_SLICE_COPY:
        case XI_BYTE_SLICE_COMPARE:
        case XI_BYTE_SLICE_COMMON_PREFIX:
            *out = arg_idx <= 1 && user->nargs > arg_idx && user->args[arg_idx]
                       ? sr_type_native_boundary_rep(user->args[arg_idx]->type)
                       : XR_REP_TAGGED;
            return true;
        case XI_BYTE_ARRAY_COPY_WITHIN:
        case XI_BYTE_ARRAY_REPEAT_FROM:
            *out = arg_idx == 0 ? XR_REP_TAGGED : XR_REP_I64;
            return true;
        case XI_BYTE_ARRAY_APPEND_FROM:
            *out = arg_idx == 0 ? XR_REP_TAGGED
                   : arg_idx == 1 && user->nargs > arg_idx && user->args[arg_idx]
                       ? sr_type_native_boundary_rep(user->args[arg_idx]->type)
                       : XR_REP_TAGGED;
            return true;
        case XI_BYTE_ARRAY_COPY_FROM:
            *out = arg_idx <= 1 ? XR_REP_TAGGED : XR_REP_I64;
            return true;
        case XI_ARRAY_DATA_PTR:
            *out = arg_idx == 0 && user->nargs >= 1 && user->args[0]
                       ? sr_type_native_boundary_rep(user->args[0]->type)
                       : XR_REP_TAGGED;
            return true;
        case XI_PTR_LOAD:
            if (arg_idx == 0) {
                *out = XR_REP_RAWPTR;
                return true;
            }
            if (arg_idx == 1 && user->nargs >= 2 && user->args[1]) {
                *out = sr_type_native_boundary_rep(user->args[1]->type);
                return true;
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_PTR_STORE:
            if (arg_idx == 0) {
                *out = XR_REP_RAWPTR;
                return true;
            }
            if (arg_idx == 1 && user->nargs >= 2 && user->args[1]) {
                *out = sr_type_native_boundary_rep(user->args[1]->type);
                return true;
            }
            if (arg_idx == 2) {
                *out = user->nargs >= 3 && user->args[2]
                           ? sr_type_native_boundary_rep(user->args[2]->type)
                           : XR_REP_TAGGED;
                return true;
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_PTR_COPY_NONOVERLAP:
            *out = arg_idx <= 1 ? XR_REP_RAWPTR : (arg_idx == 2 ? XR_REP_I64 : XR_REP_TAGGED);
            return true;
        case XI_AGG_SET:
            if (arg_idx == 1 && user->nargs >= 2 && user->args[1]) {
                *out = sr_type_scalar_rep(user->args[1]->type);
                return true;
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_LEN:
            if (arg_idx == 0 && user->nargs == 1 &&
                (sr_is_typed_array_length_field(user) ||
                 sr_is_static_collection_length_field(user))) {
                *out = sr_type_native_boundary_rep(user->args[0]->type);
                return true;
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_STORE_FIELD:
            if (arg_idx == 1 && policy && policy->prefer_call_args_native && user->nargs >= 2 &&
                sr_field_receiver_uses_native_rep(user->args[0]) && user->args[1]) {
                *out = sr_type_scalar_rep(user->args[1]->type);
                return true;
            }
            *out = XR_REP_TAGGED;
            return true;
        case XI_ARRAY_NEW:
        case XI_MAP_NEW:
        case XI_SET_NEW:
            *out = arg_idx == 0 ? XR_REP_I64 : XR_REP_TAGGED;
            return true;
        default:
            return false;
    }
}

static bool sr_use_rep_enum_op(const XiValue *user, uint16_t arg_idx, XrRep *out) {
    switch (user->op) {
        case XI_ENUM_VARIANT_AT:
        case XI_ENUM_PAYLOAD_AT:
            *out = XR_REP_I64;
            return true;
        case XI_ENUM_META_GET:
            /* The AOT-only compact payload-TypeId form repeats the descriptor
             * in arg0; the concrete enum plan replaces the VM namespace. */
            if (user->enum_metadata_field == XA_ENUM_META_PAYLOAD_TYPE &&
                user->enum_metadata_owner && user->nargs == 2 && user->args[0] == user->args[1])
                *out = XR_REP_I64;
            else
                *out = arg_idx == 1 ? XR_REP_I64 : XR_REP_TAGGED;
            return true;
        case XI_ENUM_DESCRIPTOR_BOX:
            *out = XR_REP_I64;
            return true;
        case XI_ENUM_DESCRIPTOR_UNBOX:
            *out = XR_REP_TAGGED;
            return true;
        default:
            return false;
    }
}

static bool sr_use_rep_value_op(const XiValue *user, uint16_t arg_idx, const XiRepPolicy *policy,
                                XrRep *out) {
    switch (user->op) {
        case XI_NOT:
            *out =
                arg_idx == 0 && user->args[0] ? sr_def_rep(user->args[0], policy) : XR_REP_TAGGED;
            return true;
        case XI_CONVERT:
            if (arg_idx == 0 && user->nargs >= 1 && user->args[0] &&
                !sr_convert_can_return_null(user)) {
                *out = sr_type_scalar_rep(user->args[0]->type);
                if (*out == XR_REP_TAGGED)
                    *out = sr_def_rep(user->args[0], policy);
            } else {
                *out = XR_REP_TAGGED;
            }
            return true;
        case XI_AS:
            *out = arg_idx == 0 && sr_as_is_native_numeric_width(user)
                       ? sr_type_scalar_rep(user->args[0]->type)
                       : XR_REP_TAGGED;
            return true;
        case XI_SELECT:
            if (arg_idx == 0 && user->args[0])
                *out = sr_def_rep(user->args[0], policy);
            else if (arg_idx < 3)
                *out = sr_def_rep(user, policy);
            else
                *out = XR_REP_TAGGED;
            return true;
        default:
            return false;
    }
}

static XrRep sr_use_rep(const XiValue *user, uint16_t arg_idx, const XiRepPolicy *policy) {
    XrRep mem_rep = XR_REP_TAGGED;
    if (sr_use_rep_memory_op(user, arg_idx, policy, &mem_rep))
        return mem_rep;
    XrRep enum_rep = XR_REP_TAGGED;
    if (sr_use_rep_enum_op(user, arg_idx, &enum_rep))
        return enum_rep;
    XrRep value_rep = XR_REP_TAGGED;
    if (sr_use_rep_value_op(user, arg_idx, policy, &value_rep))
        return value_rep;
    switch (user->op) {
        case XI_ADD:
        case XI_SUB:
        case XI_MUL:
        case XI_DIV:
        case XI_MOD:
        case XI_NEG:
        case XI_BAND:
        case XI_BOR:
        case XI_BXOR:
        case XI_BNOT:
        case XI_SHL:
        case XI_SHR:
        case XI_BIT_ROTL:
        case XI_BIT_ROTR:
        case XI_BIT_BSWAP:
        case XI_BIT_POPCOUNT:
        case XI_BIT_CLZ:
        case XI_BIT_MUL_HIGH:
        case XI_BIT_CTZ: {
            if ((user->op == XI_ADD || user->op == XI_SUB) && user->type &&
                user->type->kind == XR_KIND_POINTER && arg_idx < user->nargs &&
                user->args[arg_idx] && user->args[arg_idx]->type) {
                return user->args[arg_idx]->type->kind == XR_KIND_POINTER ? XR_REP_RAWPTR
                                                                          : XR_REP_I64;
            }
            return sr_arith_native_result_rep(user, policy);
        }
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
            if (sr_compare_uses_null(user))
                return XR_REP_TAGGED;
            if (arg_idx < user->nargs && user->args[arg_idx] && user->args[arg_idx]->type) {
                return sr_type_scalar_rep(user->args[arg_idx]->type);
            }
            return XR_REP_TAGGED;
        case XI_CALL:
            if (arg_idx > 0 && arg_idx < user->nargs && user->args[arg_idx] &&
                (user->args[arg_idx]->op == XI_LOCAL_ADDR ||
                 sr_param_is_call_bound_place(user->args[arg_idx])))
                return XR_REP_RAWPTR;
            if (arg_idx > 0 && policy && policy->prefer_call_args_native && arg_idx < user->nargs &&
                user->args[arg_idx]) {
                return sr_type_native_boundary_rep(user->args[arg_idx]->type);
            }
            return XR_REP_TAGGED;
        case XI_LOCAL_ADDR:
            /* A call-bound place points at the callee's native ABI storage,
             * not necessarily at the representation used by the caller's
             * source value.  Requiring the semantic type's native boundary
             * rep inserts an explicit UNBOX temporary for tagged globals,
             * captures, and phis; PLACE_LOAD then performs the writeback. */
            return arg_idx == 0 && user->args[0]
                       ? sr_type_call_place_pointee_rep(user->args[0]->type)
                       : XR_REP_TAGGED;
        case XI_PLACE_LOAD:
            return arg_idx == 0 ? XR_REP_RAWPTR : XR_REP_TAGGED;
        case XI_PLACE_STORE:
            if (arg_idx == 0)
                return XR_REP_RAWPTR;
            if (arg_idx == 1 && user->args[1])
                return sr_def_rep(user->args[1], policy);
            return XR_REP_TAGGED;
        case XI_CALL_BUILTIN: {
            const char *name = (const char *) user->aux;
            if (name && strcmp(name, "array_reserve") == 0 && arg_idx < user->nargs &&
                user->args[arg_idx]) {
                if (arg_idx == 0)
                    return sr_type_native_boundary_rep(user->args[arg_idx]->type);
                if (arg_idx == 1)
                    return XR_REP_I64;
            }
            return XR_REP_TAGGED;
        }
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            if (arg_idx < user->nargs && user->args[arg_idx] &&
                (user->args[arg_idx]->op == XI_LOCAL_ADDR ||
                 sr_param_is_call_bound_place(user->args[arg_idx])))
                return XR_REP_RAWPTR;
            /* Same rule the container operand of an index access follows: a
             * freshly allocated heap array has a single storage fact, the owned
             * tagged outer value, so a native pointer view of it as a member
             * receiver would need a representation adapter no frozen storage
             * row can state. Every other receiver keeps the native boundary it
             * already carries at its own definition. */
            if (arg_idx == 0 && sr_is_typed_array_native_receiver_method(user))
                return sr_container_operand_rep(user->args[0]);
            if (arg_idx > 0 && policy && policy->prefer_call_args_native && arg_idx < user->nargs &&
                user->args[arg_idx]) {
                return sr_type_native_boundary_rep(user->args[arg_idx]->type);
            }
            return XR_REP_TAGGED;
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
            return arg_idx == 0 ? sr_def_rep(user, policy) : XR_REP_TAGGED;
        case XI_CHAN_NEW:
            /* The immutable CHANNEL_ALLOCATION_STORAGE recipe binds the
             * capacity operand as an exact integer machine value. Keep the
             * canonical representation-selection boundary native so no BOX
             * adapter can mutate that authority. */
            return arg_idx == 0 ? XR_REP_I64 : XR_REP_TAGGED;
        case XI_BOX:
            if (user->args[0] && user->args[0]->type) {
                return sr_type_native_boundary_rep(user->args[0]->type);
            }
            return XR_REP_TAGGED;
        case XI_UNBOX:
            return XR_REP_TAGGED;
        /* NARROW/WIDEN: input must be unboxed */
        case XI_NARROW_I8:
        case XI_NARROW_U8:
        case XI_NARROW_I16:
        case XI_NARROW_U16:
        case XI_NARROW_I32:
        case XI_NARROW_U32:
        case XI_WIDEN_I8:
        case XI_WIDEN_U8:
        case XI_WIDEN_I16:
        case XI_WIDEN_U16:
        case XI_WIDEN_I32:
        case XI_WIDEN_U32:
            return XR_REP_I64;
        case XI_NARROW_F32:
        case XI_WIDEN_F32:
            return XR_REP_F64;
        default:
            return XR_REP_TAGGED;
    }
}

/* Allocate a BOX/UNBOX value in the arena without appending to the block. */
static XiValue *sr_make_convert(XiFunc *f, XiBlock *blk, uint16_t op,
                                struct XrType *type, XiValue *arg,
                                XiBackendValueOrigin origin) {
    XR_DCHECK(f != NULL, "sr_make_convert: NULL func");
    XR_DCHECK(blk != NULL, "sr_make_convert: NULL block");
    XR_DCHECK(arg != NULL, "sr_make_convert: NULL arg");
    XiValue *v = xi_value_new_unlinked(f, blk, op, type, 1);
    if (!v)
        return NULL;
    v->var_id = arg->var_id;
    v->backend_origin = (uint8_t) origin;
    v->args[0] = arg;
    v->enum_metadata_owner = arg->enum_metadata_owner;
    v->enum_metadata_field = arg->enum_metadata_field;
    v->enum_metadata_kind = arg->enum_metadata_kind;
    return v;
}

static bool sr_conversion_erases_enum_metadata(const XiValue *source, const XrType *target) {
    if (!source || !target ||
        (!xr_type_is_enum_metadata(source->type) &&
         source->enum_metadata_kind == XR_ENUM_METADATA_NONE))
        return false;
    /* Every enum-metadata target is a statically typed descriptor boundary.
     * Type checking and monomorphization have already established descriptor
     * kind/owner compatibility; wrapper pointer inequality here can merely be
     * a generic-specialization artifact.  Only an actually erased target
     * (union/interface/unknown/JSON) requires the semantic heap box. */
    if (xr_type_is_enum_metadata(target))
        return false;
    return target->kind == XR_KIND_UNION || target->kind == XR_KIND_INTERFACE ||
           target->kind == XR_KIND_JSON || target->kind == XR_KIND_UNKNOWN;
}

XR_FUNC bool xi_opt_rep_adapter_for_use(const XiValue *source, const XiValue *user,
                                        uint16_t argument_index,
                                        const XiRepPolicy *policy,
                                        XiRepAdapterKind *out_kind,
                                        uint16_t *out_input_rep,
                                        uint16_t *out_output_rep) {
    if (out_kind)
        *out_kind = XI_REP_ADAPTER_NONE;
    if (out_input_rep)
        *out_input_rep = XR_REP_TAGGED;
    if (out_output_rep)
        *out_output_rep = XR_REP_TAGGED;
    if (!source || !user || !out_kind || !out_input_rep || !out_output_rep ||
        argument_index >= user->nargs || user->args[argument_index] != source)
        return false;
    XiRepPolicy local_policy = policy ? *policy : xi_rep_policy_tagged_boundary();
    XrRep input = sr_def_rep(source, &local_policy);
    XrRep output = sr_use_rep(user, argument_index, &local_policy);
    if (input == output)
        return false;
    XiRepAdapterKind kind = XI_REP_ADAPTER_NONE;
    if (input != XR_REP_TAGGED && output == XR_REP_TAGGED) {
        bool erased = false;
        if ((user->op == XI_COPY || xi_op_is_identity_forward(user->op)) &&
            argument_index == 0)
            erased = sr_conversion_erases_enum_metadata(source, user->type);
        kind = erased ? XI_REP_ADAPTER_ENUM_DESCRIPTOR_BOX : XI_REP_ADAPTER_BOX;
    } else if (input == XR_REP_TAGGED && output != XR_REP_TAGGED) {
        kind = source->op == XI_ENUM_DESCRIPTOR_BOX
                   ? XI_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX
                   : XI_REP_ADAPTER_UNBOX;
    } else {
        return false;
    }
    *out_kind = kind;
    *out_input_rep = input;
    *out_output_rep = output;
    return true;
}

XR_FUNC bool xi_opt_rep_adapter_for_boundary(
    const XiValue *source, uint16_t required_rep, bool erase_enum_descriptor,
    const XiRepPolicy *policy, XiRepAdapterKind *out_kind,
    uint16_t *out_input_rep, uint16_t *out_output_rep) {
    if (out_kind)
        *out_kind = XI_REP_ADAPTER_NONE;
    if (out_input_rep)
        *out_input_rep = XR_REP_TAGGED;
    if (out_output_rep)
        *out_output_rep = XR_REP_TAGGED;
    if (!source || !out_kind || !out_input_rep || !out_output_rep ||
        required_rep > XR_REP_RAWPTR)
        return false;
    XiRepPolicy local_policy = policy ? *policy : xi_rep_policy_tagged_boundary();
    XrRep input = sr_def_rep(source, &local_policy);
    XrRep output = (XrRep) required_rep;
    if (input == output)
        return false;
    if (input != XR_REP_TAGGED && output == XR_REP_TAGGED)
        *out_kind = erase_enum_descriptor
                        ? XI_REP_ADAPTER_ENUM_DESCRIPTOR_BOX
                        : XI_REP_ADAPTER_BOX;
    else if (input == XR_REP_TAGGED && output != XR_REP_TAGGED)
        *out_kind = source->op == XI_ENUM_DESCRIPTOR_BOX
                        ? XI_REP_ADAPTER_ENUM_DESCRIPTOR_UNBOX
                        : XI_REP_ADAPTER_UNBOX;
    else
        return false;
    *out_input_rep = input;
    *out_output_rep = output;
    return true;
}

XR_FUNC bool xi_opt_rep_adapter_for_phi(
    const XiValue *source, const XiPhi *phi, uint16_t argument_index,
    const XiRepPolicy *policy, XiRepAdapterKind *out_kind,
    uint16_t *out_input_rep, uint16_t *out_output_rep) {
    if (!phi || argument_index >= phi->value.nargs ||
        phi->value.args[argument_index] != source)
        return false;
    XiRepPolicy local_policy = policy ? *policy : xi_rep_policy_tagged_boundary();
    XrRep required = local_policy.force_phi_tagged
                         ? XR_REP_TAGGED
                         : sr_type_native_boundary_rep(phi->value.type);
    bool erase = sr_conversion_erases_enum_metadata(source, phi->value.type);
    return xi_opt_rep_adapter_for_boundary(source, required, erase,
                                           &local_policy, out_kind,
                                           out_input_rep, out_output_rep);
}

XR_FUNC bool xi_opt_rep_adapter_for_return(
    const XiFunc *function, const XiBlock *block, const XiRepPolicy *policy,
    XiRepAdapterKind *out_kind, uint16_t *out_input_rep,
    uint16_t *out_output_rep) {
    if (!function || !block || block->func != function ||
        block->kind != XI_BLOCK_RETURN || !block->control ||
        block->control->op == XI_ERR_RETURN)
        return false;
    XiRepPolicy local_policy = policy ? *policy : xi_rep_policy_tagged_boundary();
    XrRep required = local_policy.force_return_tagged
                         ? XR_REP_TAGGED
                         : sr_type_return_boundary_rep(function->return_type);
    bool erase = sr_conversion_erases_enum_metadata(block->control,
                                                     function->return_type);
    return xi_opt_rep_adapter_for_boundary(block->control, required, erase,
                                           &local_policy, out_kind,
                                           out_input_rep, out_output_rep);
}

/* Rewrite a single arg reference if rep mismatches. */
static void sr_rewrite_arg(XiFunc *f, XiValue **arg_slot, XrRep use_r, XiValue **box_of,
                           XiValue **erased_box_of, XiValue **unbox_of, uint32_t max_id,
                           const XiRepPolicy *policy, bool erase_enum_descriptor) {
    XiValue *arg = *arg_slot;
    if (!arg || arg->id >= max_id)
        return;
    XrRep def_r = sr_def_rep(arg, policy);
    if (def_r == use_r)
        return;

    if (def_r != XR_REP_TAGGED && use_r == XR_REP_TAGGED) {
        /* Unboxed -> tagged: insert BOX */
        XiValue **cache = erase_enum_descriptor ? erased_box_of : box_of;
        uint16_t op = erase_enum_descriptor ? XI_ENUM_DESCRIPTOR_BOX : XI_BOX;
        if (!cache[arg->id]) {
            cache[arg->id] = sr_make_convert(
                f, arg->block, op, arg->type, arg,
                erase_enum_descriptor
                    ? XI_BACKEND_VALUE_ENUM_DESCRIPTOR_BOX
                    : XI_BACKEND_VALUE_REP_BOX);
        }
        if (cache[arg->id])
            *arg_slot = cache[arg->id];
    } else if (def_r == XR_REP_TAGGED && use_r != XR_REP_TAGGED) {
        /* Tagged -> unboxed: insert UNBOX */
        if (!unbox_of[arg->id]) {
            uint16_t op = arg->op == XI_ENUM_DESCRIPTOR_BOX ? XI_ENUM_DESCRIPTOR_UNBOX : XI_UNBOX;
            unbox_of[arg->id] = sr_make_convert(
                f, arg->block, op, arg->type, arg,
                op == XI_ENUM_DESCRIPTOR_UNBOX
                    ? XI_BACKEND_VALUE_ENUM_DESCRIPTOR_UNBOX
                    : XI_BACKEND_VALUE_REP_UNBOX);
        }
        if (unbox_of[arg->id])
            *arg_slot = unbox_of[arg->id];
    }
}

static void sr_rebuild_conversions(XiFunc *func, XiValue **box_of, XiValue **erased_box_of,
                                   XiValue **unbox_of, uint32_t max_id) {
    for (uint32_t bi = 0; bi < func->nblocks; bi++) {
        XiBlock *block = func->blocks[bi];
        if (!block)
            continue;

        uint32_t extra = 0;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            if (!value)
                continue;
            if (value->id < max_id && box_of[value->id] && box_of[value->id]->block == block)
                extra++;
            if (value->id < max_id && erased_box_of[value->id] &&
                erased_box_of[value->id]->block == block)
                extra++;
            if (value->id < max_id && unbox_of[value->id] && unbox_of[value->id]->block == block)
                extra++;
        }
        for (XiPhi *phi = block->phis; phi; phi = phi->next) {
            uint32_t id = phi->value.id;
            if (id < max_id && box_of[id] && box_of[id]->block == block)
                extra++;
            if (id < max_id && erased_box_of[id] && erased_box_of[id]->block == block)
                extra++;
            if (id < max_id && unbox_of[id] && unbox_of[id]->block == block)
                extra++;
        }
        if (extra == 0)
            continue;

        uint32_t new_cap = block->nvalues + extra;
        XiValue **values = (XiValue **) xi_func_arena_alloc(func, new_cap * sizeof(XiValue *));
        if (!values)
            continue;

        uint32_t count = 0;
        /* Phi-sourced conversions must precede regular instructions. */
        for (XiPhi *phi = block->phis; phi; phi = phi->next) {
            uint32_t id = phi->value.id;
            if (id < max_id && unbox_of[id] && unbox_of[id]->block == block)
                values[count++] = unbox_of[id];
            if (id < max_id && box_of[id] && box_of[id]->block == block)
                values[count++] = box_of[id];
            if (id < max_id && erased_box_of[id] && erased_box_of[id]->block == block)
                values[count++] = erased_box_of[id];
        }
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values[vi];
            values[count++] = value;
            if (!value || value->id >= max_id)
                continue;
            if (unbox_of[value->id] && unbox_of[value->id]->block == block)
                values[count++] = unbox_of[value->id];
            if (box_of[value->id] && box_of[value->id]->block == block)
                values[count++] = box_of[value->id];
            if (erased_box_of[value->id] && erased_box_of[value->id]->block == block)
                values[count++] = erased_box_of[value->id];
        }
        block->values = values;
        block->nvalues = count;
        block->values_cap = new_cap;
    }
}

XR_FUNC XiPassChange xi_opt_select_rep_with_policy(XiFunc *f, const XiRepPolicy *policy) {
    XR_DCHECK(f != NULL, "xi_opt_select_rep_with_policy: NULL func");
    XiRepPolicy local_policy = policy ? *policy : xi_rep_policy_tagged_boundary();

    uint32_t max_id = f->next_value_id;
    /* Empty module initializers are real pipeline roots.  A facade that only
     * re-exports names has no SSA values, but it must still cross the REPPED
     * stage before backend lowering.  It may also own nested functions, so do
     * not bypass the recursive stage transition. */
    if (max_id == 0) {
        for (uint16_t i = 0; i < f->nchildren; i++) {
            if (f->children[i])
                xi_opt_select_rep_with_policy(f->children[i], &local_policy);
        }
        return xi_pass_change_all();
    }

    XiValue **box_of = (XiValue **) xr_calloc(max_id, sizeof(XiValue *));
    XiValue **erased_box_of = (XiValue **) xr_calloc(max_id, sizeof(XiValue *));
    XiValue **unbox_of = (XiValue **) xr_calloc(max_id, sizeof(XiValue *));
    if (!box_of || !erased_box_of || !unbox_of) {
        xr_free(box_of);
        xr_free(erased_box_of);
        xr_free(unbox_of);
        return xi_pass_no_change();
    }

    /* Rewrite args of every instruction, phi, and block control. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            bool args_changed = false;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                XiValue *before = v->args[ai];
                XrRep use_r = sr_use_rep(v, ai, &local_policy);
                bool erase_descriptor = false;
                if ((v->op == XI_COPY || xi_op_is_identity_forward(v->op)) && ai == 0)
                    erase_descriptor = sr_conversion_erases_enum_metadata(v->args[ai], v->type);
                sr_rewrite_arg(f, &v->args[ai], use_r, box_of, erased_box_of, unbox_of, max_id,
                               &local_policy, erase_descriptor);
                args_changed = args_changed || before != v->args[ai];
            }
            if (args_changed)
                xi_value_rebase_view_evidence(v);
        }

        /* Phi args follow the selected backend policy.  VM-style consumers keep
         * merge points tagged; AOT can keep native boundary phis unboxed. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            XrRep phi_rep = local_policy.force_phi_tagged
                                ? XR_REP_TAGGED
                                : sr_type_native_boundary_rep(phi->value.type);
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                bool erase_descriptor =
                    sr_conversion_erases_enum_metadata(phi->value.args[ai], phi->value.type);
                sr_rewrite_arg(f, &phi->value.args[ai], phi_rep, box_of, erased_box_of, unbox_of,
                               max_id, &local_policy, erase_descriptor);
            }
        }

        /* Return control follows the function ABI policy. */
        if (blk->kind == XI_BLOCK_RETURN && blk->control && blk->control->op != XI_ERR_RETURN) {
            XrRep ret_rep = local_policy.force_return_tagged
                                ? XR_REP_TAGGED
                                : sr_type_return_boundary_rep(f->return_type);
            bool erase_descriptor =
                sr_conversion_erases_enum_metadata(blk->control, f->return_type);
            sr_rewrite_arg(f, &blk->control, ret_rep, box_of, erased_box_of, unbox_of, max_id,
                           &local_policy, erase_descriptor);
        }
    }

    /* Add BOX/UNBOX immediately after regular sources and prepend
     * phi-sourced conversions before ordinary block instructions. */
    sr_rebuild_conversions(f, box_of, erased_box_of, unbox_of, max_id);

    xr_free(box_of);
    xr_free(erased_box_of);
    xr_free(unbox_of);

    /* Recurse into children first (bottom-up) */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_opt_select_rep_with_policy(f->children[i], &local_policy);
    }

    /* Populate v->rep for every value and phi in this function.
     * After BOX/UNBOX insertion, sr_def_rep returns the correct
     * concrete representation for each value. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (v)
                v->rep = (uint8_t) sr_def_rep(v, &local_policy);
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next)
            phi->value.rep = (uint8_t) sr_def_rep(&phi->value, &local_policy);
    }

    return xi_pass_change_all();
}

XR_FUNC XiPassChange xi_opt_select_rep(XiFunc *f) {
    XiRepPolicy policy = xi_rep_policy_tagged_boundary();
    return xi_opt_select_rep_with_policy(f, &policy);
}

static XiFunc *sr_enum_erasure_shared_callee(const XiFunc *caller, int64_t slot) {
    if (!caller || slot < 0)
        return NULL;
    for (const XiFunc *owner = caller; owner; owner = owner->parent_func) {
        if (!owner->shared_slot_funcs || slot >= owner->shared_slot_func_count)
            continue;
        if (owner->shared_slot_funcs[slot])
            return owner->shared_slot_funcs[slot];
    }
    return NULL;
}

static XiFunc *sr_enum_erasure_callee(const XiFunc *caller, const XiValue *callee) {
    if (!callee)
        return NULL;
    if (callee->op == XI_CLOSURE_NEW && callee->aux)
        return (XiFunc *) callee->aux;
    if (callee->op == XI_GET_SHARED)
        return sr_enum_erasure_shared_callee(caller, callee->aux_int);
    if (xi_copy_is_identity_alias(callee) && callee->nargs >= 1)
        return sr_enum_erasure_callee(caller, callee->args[0]);
    return NULL;
}

static XrType *sr_enum_erasure_container_element(XrType *type) {
    if (!type)
        return NULL;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
            return type->container.element_type;
        case XR_KIND_FIXED_ARRAY:
            return type->fixed_array.element_type;
        default:
            return NULL;
    }
}

static XrType *sr_enum_erasure_shared_type(const XiFunc *f, int64_t slot) {
    if (!f || slot < 0)
        return NULL;
    XrType *observed = NULL;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->op != XI_GET_SHARED || v->aux_int != slot || !v->type)
                continue;
            if (v->type->kind == XR_KIND_UNION || v->type->kind == XR_KIND_INTERFACE ||
                v->type->kind == XR_KIND_JSON || v->type->kind == XR_KIND_UNKNOWN)
                return v->type;
            if (!observed)
                observed = v->type;
        }
    }
    return observed;
}

static XrType *sr_enum_erasure_arg_target(XiFunc *f, XiValue *user, uint16_t arg_index) {
    if (!user || arg_index >= user->nargs)
        return NULL;
    switch (user->op) {
        case XI_COPY:
        case XI_SOURCE_MOVE:
        case XI_OWNER_FORWARD:
            return arg_index == 0 ? user->type : NULL;
        case XI_SET_SHARED:
            return arg_index == 0 ? sr_enum_erasure_shared_type(f, user->aux_int) : NULL;
        case XI_STORE_UPVAL:
            return arg_index == 0 && user->aux_int >= 0 && user->aux_int < (int64_t) f->ncaptures
                       ? f->captures[user->aux_int].type
                       : NULL;
        case XI_INDEX_SET:
            if (arg_index != 2 || !user->args[0] || !user->args[0]->type)
                return NULL;
            return user->args[0]->type->kind == XR_KIND_MAP
                       ? user->args[0]->type->map.value_type
                       : sr_enum_erasure_container_element(user->args[0]->type);
        case XI_ARRAY_PUSH:
            return arg_index == 1 && user->args[0]
                       ? sr_enum_erasure_container_element(user->args[0]->type)
                       : NULL;
        case XI_CHAN_SEND:
        case XI_CHAN_TRY_SEND:
            return arg_index == 1 && user->args[0]
                       ? sr_enum_erasure_container_element(user->args[0]->type)
                       : NULL;
        case XI_CALL:
        case XI_TAIL_CALL:
            if (arg_index > 0 && user->args[0]) {
                XiFunc *callee = sr_enum_erasure_callee(f, user->args[0]);
                uint16_t param = (uint16_t) (arg_index - 1);
                if (callee && param < callee->nparams && callee->params[param])
                    return callee->params[param]->type;
                XrType *fn_type = user->args[0]->type;
                if (fn_type && fn_type->kind == XR_KIND_FUNCTION &&
                    param < (uint16_t) fn_type->function.param_count)
                    return fn_type->function.params[param].type;
            }
            return NULL;
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            if (arg_index == 1 && user->args[0] && xi_opt_method_name_is(user, "push", SYMBOL_PUSH))
                return sr_enum_erasure_container_element(user->args[0]->type);
            if (arg_index == 1 && user->args[0] && user->args[0]->type &&
                user->args[0]->type->kind == XR_KIND_CHANNEL) {
                const char *method = user->aux ? (const char *) user->aux : NULL;
                if (method && (strcmp(method, "send") == 0 || strcmp(method, "trySend") == 0 ||
                               strcmp(method, "sendTimeout") == 0))
                    return sr_enum_erasure_container_element(user->args[0]->type);
            }
            return NULL;
        default:
            return NULL;
    }
}

/* Materialize the semantic allocation required when a compact, typed enum
 * descriptor crosses an erased identity boundary.  This runs before escape
 * and ownership analysis on both VM and AOT paths; ordinary representation
 * BOX/UNBOX remains an AOT-only concern. */
XR_FUNC XiPassChange xi_opt_materialize_enum_descriptor_erasure(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_materialize_enum_descriptor_erasure: NULL func");

    uint32_t max_id = f->next_value_id;
    if (max_id == 0) {
        XiPassChange nested = xi_pass_no_change();
        for (uint16_t i = 0; i < f->nchildren; i++) {
            if (!f->children[i])
                continue;
            XiPassChange child = xi_opt_materialize_enum_descriptor_erasure(f->children[i]);
            if (child.cfg_changed || child.values_changed || child.types_changed)
                nested = xi_pass_change_all();
        }
        return nested;
    }

    XiValue **box_of = (XiValue **) xr_calloc(max_id, sizeof(XiValue *));
    if (!box_of)
        return xi_pass_no_change();
    bool changed = false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->nargs == 0)
                continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                XiValue *source = v->args[ai];
                XrType *target = sr_enum_erasure_arg_target(f, v, ai);
                if (!source || !target || source->op == XI_ENUM_DESCRIPTOR_BOX ||
                    source->id >= max_id || !sr_conversion_erases_enum_metadata(source, target))
                    continue;
                if (!box_of[source->id])
                    box_of[source->id] =
                        sr_make_convert(f, source->block,
                                        XI_ENUM_DESCRIPTOR_BOX, target, source,
                                        XI_BACKEND_VALUE_NONE);
                if (box_of[source->id]) {
                    v->args[ai] = box_of[source->id];
                    changed = true;
                }
            }
        }

        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                XiValue *source = phi->value.args[ai];
                if (!source || source->op == XI_ENUM_DESCRIPTOR_BOX || source->id >= max_id ||
                    !sr_conversion_erases_enum_metadata(source, phi->value.type))
                    continue;
                if (!box_of[source->id])
                    box_of[source->id] = sr_make_convert(
                        f, source->block, XI_ENUM_DESCRIPTOR_BOX,
                        phi->value.type, source, XI_BACKEND_VALUE_NONE);
                if (box_of[source->id]) {
                    phi->value.args[ai] = box_of[source->id];
                    changed = true;
                }
            }
        }

        if (blk->kind == XI_BLOCK_RETURN && blk->control && blk->control->op != XI_ERR_RETURN &&
            blk->control->op != XI_ENUM_DESCRIPTOR_BOX && blk->control->id < max_id &&
            sr_conversion_erases_enum_metadata(blk->control, f->return_type)) {
            XiValue *source = blk->control;
            if (!box_of[source->id])
                box_of[source->id] = sr_make_convert(
                    f, source->block, XI_ENUM_DESCRIPTOR_BOX,
                    f->return_type, source, XI_BACKEND_VALUE_NONE);
            if (box_of[source->id]) {
                blk->control = box_of[source->id];
                changed = true;
            }
        }
    }

    if (changed) {
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            XiBlock *blk = f->blocks[bi];
            if (!blk)
                continue;
            uint32_t extra = 0;
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                XiValue *v = blk->values[vi];
                if (v && v->id < max_id && box_of[v->id] && box_of[v->id]->block == blk)
                    extra++;
            }
            for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
                uint32_t id = phi->value.id;
                if (id < max_id && box_of[id] && box_of[id]->block == blk)
                    extra++;
            }
            if (extra == 0)
                continue;
            XiValue **values =
                (XiValue **) xi_func_arena_alloc(f, (blk->nvalues + extra) * sizeof(XiValue *));
            if (!values)
                continue;
            uint32_t out = 0;
            for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
                uint32_t id = phi->value.id;
                if (id < max_id && box_of[id] && box_of[id]->block == blk)
                    values[out++] = box_of[id];
            }
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                XiValue *v = blk->values[vi];
                values[out++] = v;
                if (v && v->id < max_id && box_of[v->id] && box_of[v->id]->block == blk)
                    values[out++] = box_of[v->id];
            }
            blk->values = values;
            blk->nvalues = out;
            blk->values_cap = out;
        }
    }
    xr_free(box_of);

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (!f->children[i])
            continue;
        XiPassChange child = xi_opt_materialize_enum_descriptor_erasure(f->children[i]);
        if (child.cfg_changed || child.values_changed || child.types_changed)
            changed = true;
    }
    return changed ? xi_pass_change_all() : xi_pass_no_change();
}

/* The VM bytecode path intentionally lowers a concrete generic enum payload
 * TypeId lookup to a finite XI_SELECT chain: its runtime enum namespace is
 * declaration-shaped and cannot encode every generic specialization.  AOT,
 * however, owns a verified, concrete XaotEnumPlan with substituted payload
 * types.  Collapse that VM-oriented chain before AOT representation selection
 * so large enums produce one cold table lookup instead of thousands of SSA
 * compares/selects. */
static XiValue *enum_payload_type_select_descriptor(XiValue *value) {
    if (!value || value->op != XI_SELECT || value->nargs != 3 || !value->args[0])
        return NULL;
    XiValue *condition = value->args[0];
    if (condition->op != XI_EQ || condition->nargs != 2 || !condition->args[0] ||
        !condition->args[1])
        return NULL;
    int64_t ignored = 0;
    if (const_int_value(condition->args[0], &ignored))
        return condition->args[1];
    if (const_int_value(condition->args[1], &ignored))
        return condition->args[0];
    return NULL;
}

XR_FUNC XiPassChange xi_opt_compact_enum_payload_type_lookup(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_compact_enum_payload_type_lookup: NULL func");
    XiPassChange changed = xi_pass_no_change();

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *block = f->blocks ? f->blocks[bi] : NULL;
        if (!block)
            continue;
        for (uint32_t vi = 0; vi < block->nvalues; vi++) {
            XiValue *value = block->values ? block->values[vi] : NULL;
            if (!value || value->enum_metadata_field != XA_ENUM_META_PAYLOAD_TYPE ||
                !value->enum_metadata_owner || value->enum_metadata_owner->kind != XR_KIND_ENUM ||
                !value->enum_metadata_owner->enum_type.layout)
                continue;
            XiValue *descriptor = enum_payload_type_select_descriptor(value);
            if (!descriptor)
                continue;
            value->op = XI_ENUM_META_GET;
            value->args[0] = descriptor;
            value->args[1] = descriptor;
            value->nargs = 2;
            value->aux_int = XA_ENUM_META_PAYLOAD_TYPE;
            value->aux = NULL;
            value->aux_kind = XI_AUX_KIND_NONE;
            value->flags = xi_op_default_effects(XI_ENUM_META_GET);
            value->mem_group = (uint8_t) xi_tbaa_group_for_op(value->op);
            changed.values_changed = true;
        }
    }

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (!f->children[i])
            continue;
        XiPassChange child = xi_opt_compact_enum_payload_type_lookup(f->children[i]);
        if (child.cfg_changed || child.values_changed || child.types_changed)
            changed = xi_pass_change_all();
    }
    return changed;
}

/* ========== BOX/UNBOX Peephole Elimination ========== */

/*
 * Fold IF blocks whose condition became a constant, then isolate the blocks
 * that lose their last predecessor.  This pass owns the constant it
 * introduced in fold_null_test_over_untagged_box, and it runs after the
 * semantic optimizer, so no SCCP round is left to consume it.  Every caller of
 * representation selection runs box_elim, but not all of them follow with a
 * general DCE, so an uncleaned dead arm would reach codegen as unreachable
 * statements with locals of their own.  Same rewrite shape as SCCP's
 * constant-branch case, but legal at STAGE_REPPED because it needs no lattice.
 */
static bool box_elim_const_control_value(const XiValue *v, bool *taken) {
    while (v && xi_copy_is_identity_alias(v) && v->nargs >= 1)
        v = v->args[0];
    if (!v || v->op != XI_CONST || !v->type)
        return false;
    if (v->type->kind != XR_KIND_BOOL && !XR_TYPE_IS_INT(v->type))
        return false;
    *taken = v->aux_int != 0;
    return true;
}

static XiPassChange box_elim_fold_const_branches(XiFunc *f) {
    XiPassChange chg = xi_pass_no_change();
    bool changed = true;

    while (changed) {
        changed = false;
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            XiBlock *blk = f->blocks[bi];
            bool taken_edge0 = false;
            if (!blk || blk->kind != XI_BLOCK_IF ||
                !box_elim_const_control_value(blk->control, &taken_edge0))
                continue;

            XiBlock *taken = taken_edge0 ? blk->succs[0] : blk->succs[1];
            XiBlock *dropped = taken_edge0 ? blk->succs[1] : blk->succs[0];
            if (dropped && dropped != taken) {
                while (xi_cfg_remove_pred(dropped, blk)) {
                }
            }
            blk->kind = XI_BLOCK_PLAIN;
            blk->succs[0] = taken;
            blk->succs[1] = NULL;
            blk->control = NULL;
            blk->line = 0;
            changed = true;
            chg.cfg_changed = true;
        }

        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            if (xi_cfg_mark_unreachable_if_isolated(f, f->blocks[bi])) {
                changed = true;
                chg.cfg_changed = true;
            }
        }
    }

    uint32_t removed = xi_cfg_compact_blocks(f);
    if (removed > 0) {
        chg.cfg_changed = true;
        chg.n_removed += removed;
    }
    return chg;
}

/* Delete the boxing adapters that fold_null_test_over_untagged_box orphaned.
 * Scoped to XI_BOX so this stays the pass cleaning up after itself rather than
 * a second dead-code eliminator: an unused pure box is dead by construction,
 * and leaving it behind makes codegen declare a C local it never assigns. */
static XiPassChange box_elim_drop_dead_boxes(XiFunc *f) {
    XiPassChange chg = xi_pass_no_change();
    bool changed = true;

    while (changed) {
        changed = false;
        compute_use_counts(f);
        for (uint32_t b = 0; b < f->nblocks; b++) {
            XiBlock *blk = f->blocks[b];
            for (uint32_t i = 0; i < blk->nvalues;) {
                XiValue *v = blk->values[i];
                if (!v || v->op != XI_BOX || v->uses > 0 ||
                    (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_MAY_THROW))) {
                    i++;
                    continue;
                }
                for (uint16_t a = 0; a < v->nargs; a++) {
                    if (v->args[a])
                        v->args[a]->uses--;
                }
                block_remove_value(blk, i);
                changed = true;
                chg.values_changed = true;
                chg.n_removed++;
            }
        }
    }
    return chg;
}

/*
 * A null test over a boxing adapter whose source already carries an untagged
 * scalar representation is statically false: XR_FROM_INT/XR_FROM_FLOAT/
 * XR_FROM_BOOL cannot produce XR_TAG_NULL.  Only representation selection can
 * create this shape, so the fold belongs here rather than in the semantic
 * peephole.  It is what a guard-fused container read leaves behind: the
 * containsKey guard lets select_rep give `get` the native i64 value rep, the
 * force-unwrap still asks whether the result is null, and answering that
 * question is the sole reason the box exists.  Folding the test lets the two
 * cleanups above delete both, keeping the guarded path entirely untagged.
 *
 * PTR is excluded on purpose: a boxed managed reference preserves a null
 * pointer, so its null test is a real runtime question.
 */
static bool fold_null_test_over_untagged_box(XiValue *v) {
    const XiValue *src;
    if (!v || v->op != XI_ISNULL || v->nargs != 1 || !v->args[0])
        return false;
    src = v->args[0];
    if (src->op != XI_BOX || src->nargs != 1 || !src->args[0])
        return false;
    src = src->args[0];
    switch ((XrRep) src->rep) {
        case XR_REP_I64:
        case XR_REP_F64:
            break;
        default:
            return false;
    }
    rewrite_to_const_int(v, xr_null_test_tagged_core(UINT8_C(1)) ? 1 : 0);
    return true;
}

/*
 * Eliminate inverse BOX/UNBOX pairs:
 *   UNBOX(BOX(x)) -> COPY(x)
 *   BOX(UNBOX(x)) -> COPY(x)
 * Subsequent copy-prop and DCE clean up the COPY and dead BOX/UNBOX.
 */
XR_FUNC XiPassChange xi_opt_box_elim(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_box_elim: NULL func");
    XiPassChange chg = xi_pass_no_change();
    bool folded_null_test = false;

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->nargs < 1 || !v->args[0])
                continue;

            if (fold_null_test_over_untagged_box(v)) {
                chg.values_changed = true;
                folded_null_test = true;
                continue;
            }

            XiValue *inner = v->args[0];
            if (inner->nargs < 1 || !inner->args[0])
                continue;

            bool elim =
                (v->op == XI_UNBOX && inner->op == XI_BOX) ||
                (v->op == XI_BOX && inner->op == XI_UNBOX) ||
                (v->op == XI_ENUM_DESCRIPTOR_UNBOX && inner->op == XI_ENUM_DESCRIPTOR_BOX) ||
                (v->op == XI_ENUM_DESCRIPTOR_BOX && inner->op == XI_ENUM_DESCRIPTOR_UNBOX);
            if (elim) {
                rewrite_to_copy(v, inner->args[0]);
                chg.values_changed = true;
            }
        }
    }

    if (folded_null_test) {
        chg = xi_pass_merge(chg, box_elim_fold_const_branches(f));
        chg = xi_pass_merge(chg, box_elim_drop_dead_boxes(f));
    }

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_opt_box_elim(f->children[i]);
    }
    return chg;
}

static XiPassChange refresh_rep_cleanup_recursive(XiFunc *f) {
    if (!f)
        return xi_pass_no_change();
    XiPassChange changed = xi_opt_copy_prop(f);
    changed = xi_pass_merge(changed, xi_opt_dce(f));
    for (uint16_t i = 0; i < f->nchildren; i++)
        changed = xi_pass_merge(changed, refresh_rep_cleanup_recursive(f->children[i]));
    return changed;
}

XR_FUNC XiPassChange xi_opt_refresh_representations_with_policy(XiFunc *f,
                                                                const XiRepPolicy *policy) {
    XR_DCHECK(f != NULL, "xi_opt_refresh_representations_with_policy: NULL func");
    XiPassChange changed = xi_opt_select_rep_with_policy(f, policy);
    changed = xi_pass_merge(changed, xi_opt_box_elim(f));
    changed = xi_pass_merge(changed, refresh_rep_cleanup_recursive(f));
    return changed;
}

/* ========== Combined Pass ========== */

XR_FUNC void xi_opt_run(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_run: NULL func");
    xi_opt_run_pipeline(f, XI_OPT_LIGHT);
}

/* ========== Pipeline Driver ========== */

/* Every pass declares an evidence policy. XiEditSession derives the precise
 * invalidation set from audited CFG/value/type/memory/call fingerprints;
 * analysis passes preserve IR revisions and publish fresh proof. */
#define XI_REWRITE_PASS(pass_name, pass_fn, level, pass_flags, required_evidence)                  \
    {                                                                                              \
        .name = pass_name,                                                                         \
        .fn = pass_fn,                                                                             \
        .min_level = level,                                                                        \
        .flags = pass_flags,                                                                       \
        .min_stage = XI_STAGE_RAW,                                                                 \
        .max_stage = XI_STAGE_CORO_LOWERED,                                                        \
        .requires_inv_mask = 0,                                                                    \
        .produces_inv_mask = 0,                                                                    \
        .requires_evidence = required_evidence,                                                    \
        .produces_evidence = 0,                                                                    \
        .invalidates_evidence = 0,                                                                 \
        .preserves_evidence = 0,                                                                   \
    }

#define XI_ANALYSIS_PASS(pass_name, pass_fn, level, pass_flags, produced_evidence)                 \
    {                                                                                              \
        .name = pass_name,                                                                         \
        .fn = pass_fn,                                                                             \
        .min_level = level,                                                                        \
        .flags = (pass_flags) | XI_PASS_CORO_PLAN_SAFE,                                            \
        .min_stage = XI_STAGE_RAW,                                                                 \
        .max_stage = XI_STAGE_CORO_LOWERED,                                                        \
        .requires_inv_mask = 0,                                                                    \
        .produces_inv_mask = 0,                                                                    \
        .requires_evidence = 0,                                                                    \
        .produces_evidence = produced_evidence,                                                    \
        .invalidates_evidence = 0,                                                                 \
        .preserves_evidence = XI_EVD_ALL,                                                          \
    }

/* Pass table: ordered by recommended execution sequence.
 * The driver runs all passes whose min_level <= requested level. */
static const XiPassDesc xi_pass_table[] = {
    XI_ANALYSIS_PASS("tbaa", xi_tbaa_annotate, XI_OPT_LIGHT, XI_PASS_REQUIRED, XI_EVD_ALIAS),
    XI_REWRITE_PASS("constfold", xi_opt_const_fold, XI_OPT_LIGHT, XI_PASS_CORO_PLAN_SAFE, 0),
    XI_REWRITE_PASS("strength_reduce", xi_opt_strength_reduce, XI_OPT_LIGHT,
                    XI_PASS_CORO_PLAN_SAFE, 0),
    XI_REWRITE_PASS("copy_prop", xi_opt_copy_prop, XI_OPT_LIGHT, XI_PASS_CORO_PLAN_SAFE, 0),
    XI_REWRITE_PASS("mark_one_shot_await", xi_opt_mark_one_shot_await, XI_OPT_LIGHT, XI_PASS_NONE,
                    0),
    XI_REWRITE_PASS("phi_simplify", xi_opt_phi_simplify, XI_OPT_LIGHT, XI_PASS_NONE, 0),
    XI_REWRITE_PASS("dce", xi_opt_dce, XI_OPT_LIGHT, XI_PASS_NONE, 0),
    XI_REWRITE_PASS("sccp", xi_opt_sccp, XI_OPT_FULL, XI_PASS_NONE, 0),
    XI_ANALYSIS_PASS("range", xi_range_analyze, XI_OPT_FULL, XI_PASS_NONE, XI_EVD_RANGE),
    XI_REWRITE_PASS("bce", xi_opt_bce, XI_OPT_FULL, XI_PASS_NONE, XI_EVD_RANGE),
    XI_REWRITE_PASS("gvn", xi_opt_gvn_pre, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_EVD_ALIAS),
    XI_REWRITE_PASS("loop_rotate", xi_opt_loop_rotate, XI_OPT_FULL,
                    XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP, 0),
    XI_REWRITE_PASS("licm", xi_opt_licm, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_EVD_ALIAS),
    XI_REWRITE_PASS("ivsr", xi_opt_ivsr, XI_OPT_FULL, XI_PASS_NEEDS_DOM, 0),
    XI_REWRITE_PASS("loop_peel", xi_opt_loop_peel, XI_OPT_FULL,
                    XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP, 0),
    XI_REWRITE_PASS("loop_unroll", xi_opt_loop_unroll, XI_OPT_FULL,
                    XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP, 0),
    XI_REWRITE_PASS("loop_split", xi_opt_loop_split, XI_OPT_FULL,
                    XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP, 0),
    XI_REWRITE_PASS("loop_inv_branch", xi_opt_loop_inv_branch, XI_OPT_FULL,
                    XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP, 0),
    XI_REWRITE_PASS("inline", xi_opt_inline, XI_OPT_FULL, XI_PASS_NONE, 0),
    XI_REWRITE_PASS("tail_call", xi_opt_tail_call, XI_OPT_FULL, XI_PASS_NONE, 0),
    XI_REWRITE_PASS("ifconv", xi_opt_ifconv, XI_OPT_FULL, XI_PASS_NEEDS_DOM, 0),
    XI_REWRITE_PASS("jump_thread", xi_opt_jump_thread, XI_OPT_FULL, XI_PASS_NEEDS_DOM, 0),
    XI_REWRITE_PASS("block_simplify", xi_opt_block_simplify, XI_OPT_FULL, XI_PASS_NONE, 0),
    XI_REWRITE_PASS("const_fixpoint", xi_opt_const_fixpoint, XI_OPT_FULL, XI_PASS_NONE, 0),
};

#undef XI_ANALYSIS_PASS
#undef XI_REWRITE_PASS

#define XI_PASS_TABLE_SIZE (sizeof(xi_pass_table) / sizeof(xi_pass_table[0]))

/* Pass names in XI_OPT_PASS_LIST order.  The disable mask addresses passes by
 * table index, so this list and the table must stay in lockstep. */
static const char *const xi_opt_pass_id_names[] = {
#define XI_OPT_PASS_NAME_ENTRY(upper, lower) lower,
    XI_OPT_PASS_LIST(XI_OPT_PASS_NAME_ENTRY)
#undef XI_OPT_PASS_NAME_ENTRY
};

XR_STATIC_ASSERT(XI_OPT_PASS_ID_COUNT <= 32,
                 "XiOptDisableMask is 32 bits wide; the pass list outgrew it");
XR_STATIC_ASSERT(sizeof(xi_opt_pass_id_names) / sizeof(xi_opt_pass_id_names[0]) ==
                     (size_t) XI_OPT_PASS_ID_COUNT,
                 "pass id list and pass name list disagree");

/* Validate pass table invariants at startup. */
static void validate_pass_table_once(void) {
    XR_CHECK(XI_PASS_TABLE_SIZE == (size_t) XI_OPT_PASS_ID_COUNT,
             "pass table size does not match the pass id list");
    for (size_t i = 0; i < XI_PASS_TABLE_SIZE; i++) {
        const XiPassDesc *d = &xi_pass_table[i];
        (void) d;
        XR_DCHECK(d->name != NULL, "pass table entry has NULL name");
        XR_DCHECK(d->fn != NULL, "pass table entry has NULL fn");
        XR_DCHECK(d->max_stage >= d->min_stage, "pass has an invalid stage window");
        XR_CHECK(strcmp(d->name, xi_opt_pass_id_names[i]) == 0,
                 "pass table order diverged from the pass id list");
    }

    /* Check declarative pass ordering constraints */
    XR_CHECK(xi_pass_order_check(), "xi pass order check failed");
}

static xr_once_t validate_pass_table_once_token = XR_ONCE_INITIALIZER;

static void validate_pass_table(void) {
    xr_once_call(&validate_pass_table_once_token, validate_pass_table_once);
}

static int xi_check_per_pass_enabled = 0;
static xr_once_t xi_check_per_pass_once = XR_ONCE_INITIALIZER;

static void init_xi_check_per_pass(void) {
    const char *env = getenv("XRAY_XI_CHECK");
    xi_check_per_pass_enabled = (env && env[0] == '1') ? 1 : 0;
}

/* ========== Pass Order Constraints ========== */

/* Declarative ordering rules. Each entry says "before must appear
 * earlier than after in the pass table".  Checked once at startup. */
static const XiPassOrderConstraint xi_pass_constraints[] = {
    /* Within the same opt level, ordering matters for efficiency.
     * Cross-level constraints (e.g. SCCP -> DCE) are not enforced
     * here because the fixed-point loop handles convergence. */
    {"constfold", "copy_prop", "constant folding enables more copy propagation"},
    {"copy_prop", "mark_one_shot_await", "copy propagation exposes local go/await pairs"},
    {"mark_one_shot_await", "dce", "one-shot await marking must see live local task uses"},
    {"gvn", "licm", "GVN eliminates redundancies before LICM hoists"},
    {"loop_rotate", "licm", "loop rotation exposes a canonical loop body before hoisting"},
};

#define XI_CONSTRAINT_COUNT (sizeof(xi_pass_constraints) / sizeof(xi_pass_constraints[0]))

/* Return the index of a pass by name, or -1 if not found. */
static int pass_index_by_name(const char *name) {
    for (size_t i = 0; i < XI_PASS_TABLE_SIZE; i++) {
        if (strcmp(xi_pass_table[i].name, name) == 0)
            return (int) i;
    }
    return -1;
}

static const char *xi_dump_func = NULL;
static const char *xi_dump_pass = NULL;
static char xi_dump_func_buf[64];
static char xi_dump_pass_buf[64];
static xr_once_t xi_dump_once = XR_ONCE_INITIALIZER;

static void init_xi_dump_config(void) {
    const char *dump_env = getenv("XRAY_XI_DUMP");
    if (!dump_env || !dump_env[0])
        return;

    const char *colon = strchr(dump_env, ':');
    if (!colon)
        return;

    size_t flen = (size_t) (colon - dump_env);
    if (flen >= sizeof(xi_dump_func_buf))
        flen = sizeof(xi_dump_func_buf) - 1;
    memcpy(xi_dump_func_buf, dump_env, flen);
    xi_dump_func_buf[flen] = '\0';
    xi_dump_func = xi_dump_func_buf;
    strncpy(xi_dump_pass_buf, colon + 1, sizeof(xi_dump_pass_buf) - 1);
    xi_dump_pass_buf[sizeof(xi_dump_pass_buf) - 1] = '\0';
    xi_dump_pass = xi_dump_pass_buf;
}

static int xi_shuffle_blocks_enabled = 0;
static xr_once_t xi_shuffle_once = XR_ONCE_INITIALIZER;

static void init_xi_shuffle_config(void) {
    const char *env = getenv("XRAY_XI_SHUFFLE");
    xi_shuffle_blocks_enabled = (env && env[0] == '1') ? 1 : 0;
    if (!xi_shuffle_blocks_enabled)
        return;

    const char *seed_env = getenv("XRAY_XI_SHUFFLE_SEED");
    unsigned seed = (seed_env && seed_env[0]) ? (unsigned) strtoul(seed_env, NULL, 10) : 0;
    if (seed == 0)
        seed = (unsigned) xr_time_monotonic_ns();
    srand(seed);
}

typedef struct XiPassRuntimeConfig {
    bool dump;
} XiPassRuntimeConfig;

static XiPassRuntimeConfig xi_pass_cfg[XI_PASS_TABLE_SIZE];

/* Passes switched off through XRAY_XI_PASS.  Merged into the caller's mask so
 * the environment and a pipeline configuration reach the driver by one path. */
static XiOptDisableMask xi_pass_env_disable_mask = XI_OPT_DISABLE_NONE;
static xr_once_t xi_pass_cfg_once = XR_ONCE_INITIALIZER;

static void init_xi_pass_config(void) {
    memset(xi_pass_cfg, 0, sizeof(xi_pass_cfg));
    xi_pass_env_disable_mask = XI_OPT_DISABLE_NONE;
    const char *env = getenv("XRAY_XI_PASS");
    if (!env || !env[0])
        return;

    /* The whole pass list can be named at once, so the request is copied to a
     * heap buffer rather than truncated into a fixed one. */
    size_t len = strlen(env);
    char *buf = (char *) xr_malloc(len + 1);
    if (!buf) {
        fprintf(stderr, "[xi_pass] error: out of memory parsing XRAY_XI_PASS\n");
        return;
    }
    memcpy(buf, env, len + 1);

    char *save = NULL;
    char *tok = strtok_r(buf, ",", &save);
    while (tok) {
        char *colon = strchr(tok, ':');
        if (colon) {
            *colon = '\0';
            const char *pname = tok;
            const char *kv = colon + 1;
            int idx = pass_index_by_name(pname);
            if (idx >= 0) {
                if (strcmp(kv, "enable=0") == 0)
                    xi_pass_env_disable_mask |= XI_OPT_DISABLE_BIT(idx);
                else if (strcmp(kv, "dump=1") == 0)
                    xi_pass_cfg[idx].dump = true;
                else
                    fprintf(stderr,
                            "[xi_pass] warning: unknown setting '%s' for pass '%s' "
                            "in XRAY_XI_PASS\n",
                            kv, pname);
            } else {
                fprintf(stderr,
                        "[xi_pass] warning: unknown pass '%s' "
                        "in XRAY_XI_PASS\n",
                        pname);
            }
        }
        tok = strtok_r(NULL, ",", &save);
    }
    xr_free(buf);
}

XR_FUNC bool xi_pass_order_check(void) {
    for (size_t c = 0; c < XI_CONSTRAINT_COUNT; c++) {
        const XiPassOrderConstraint *pc = &xi_pass_constraints[c];
        int bi = pass_index_by_name(pc->before);
        int ai = pass_index_by_name(pc->after);

        /* If either pass is not in the table (e.g. external pass),
         * the constraint is trivially satisfied. */
        if (bi < 0 || ai < 0)
            continue;

        if (bi >= ai) {
            fprintf(stderr,
                    "[xi_pass] order violation: '%s' (index %d) must "
                    "precede '%s' (index %d) — %s\n",
                    pc->before, bi, pc->after, ai, pc->reason);
            return false;
        }
    }

    for (size_t i = 0; i < XI_PASS_TABLE_SIZE; i++) {
        const XiPassDesc *d = &xi_pass_table[i];
        XiInvariantMask available = 0;
        for (size_t j = 0; j < i; j++) {
            const XiPassDesc *prev = &xi_pass_table[j];
            if (prev->min_level <= d->min_level)
                available |= prev->produces_inv_mask;
        }

        XiInvariantMask missing = d->requires_inv_mask & ~available;
        if (missing) {
            fprintf(stderr,
                    "[xi_pass] invariant order violation: '%s' requires "
                    "0x%x before the pass table has produced it\n",
                    d->name, missing);
            return false;
        }
    }

    return true;
}

/* Find or create a stats slot for the given pass name. */
static XiPassStats *stats_slot(XiPipelineStats *st, const char *name) {
    if (!st)
        return NULL;
    for (uint32_t i = 0; i < st->npass; i++) {
        if (st->passes[i].name == name)
            return &st->passes[i];
    }
    if (st->npass >= XI_MAX_PASS_STATS)
        return NULL;
    XiPassStats *s = &st->passes[st->npass++];
    memset(s, 0, sizeof(*s));
    s->name = name;
    return s;
}

/* A pass is switched off when the bit at its table index is set.  Required
 * passes are structural and ignore the mask. */
static bool pass_disabled_by_mask(const XiPassDesc *desc, size_t pass_index,
                                  XiOptDisableMask disabled_passes) {
    if (!desc || (desc->flags & XI_PASS_REQUIRED))
        return false;
    return (disabled_passes & XI_OPT_DISABLE_BIT(pass_index)) != 0;
}

static void stats_merge(XiPipelineStats *dst, const XiPipelineStats *src) {
    if (!dst || !src)
        return;
    for (uint32_t i = 0; i < src->npass; i++) {
        const XiPassStats *sp = &src->passes[i];
        XiPassStats *dp = stats_slot(dst, sp->name);
        if (!dp)
            continue;
        dp->invocations += sp->invocations;
        dp->n_removed += sp->n_removed;
        dp->n_added += sp->n_added;
        dp->elapsed_ns += sp->elapsed_ns;
    }
    dst->total_rounds += src->total_rounds;
    dst->total_ns += src->total_ns;
    dst->rpo_recomputes += src->rpo_recomputes;
    dst->dom_recomputes += src->dom_recomputes;
    dst->loop_recomputes += src->loop_recomputes;
}

/* Flags whose relative order must be preserved within a block.
 * READS_MEM is included so a read cannot move before a preceding write. */
#define XI_SHUFFLE_EFFECT_MASK                                                                     \
    (XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM | XI_FLAG_MAY_THROW |            \
     XI_FLAG_MAY_SUSPEND)

/* Returns true if value v participates in the emit-time var coalescing chain.
 * Any value with a var_id is a destructive update of that user variable, and
 * any value reading a same-block value or phi that has a var_id is a read
 * of that variable's current SSA register.  The emit pipeline coalesces all
 * SSA versions of the same var_id into a single VM register, so swapping
 * "read of old version" past "destructive update" silently changes which
 * value the register holds — exactly the kind of corruption the shuffle
 * mode is designed to expose, but only valid if shuffle itself respects
 * the implicit ordering.  Treating var-touching values as effectful chains
 * them all in original order, which is correct (just less aggressive than
 * the data-flow-only model used previously). */
static bool shuffle_touches_var(const XiBlock *blk, const XiValue *v) {
    if (!v)
        return false;
    if (xi_var_id_is_valid(v->var_id))
        return true;
    for (uint16_t a = 0; a < v->nargs; a++) {
        XiValue *arg = v->args[a];
        if (!arg)
            continue;
        /* Read of a same-block SSA value that itself carries a var_id, or
         * a phi belonging to this block that does so. */
        if (arg->block == blk && xi_var_id_is_valid(arg->var_id))
            return true;
        if (arg->op == XI_PHI && arg->block == blk && xi_var_id_is_valid(arg->var_id))
            return true;
    }
    return false;
}

static bool shuffle_is_effectful(const XiBlock *blk, const XiValue *v) {
    if (v->flags & XI_SHUFFLE_EFFECT_MASK)
        return true;
    return shuffle_touches_var(blk, v);
}

/* Count incoming edges for each value: intra-block data deps + effect chain.
 * Phi values live on blk->phis, not blk->values, so they are filtered out. */
static void shuffle_count_indeg(const XiBlock *blk, uint32_t *indeg) {
    uint32_t n = blk->nvalues;
    memset(indeg, 0, n * sizeof(uint32_t));
    int32_t prev_effect = -1;
    for (uint32_t i = 0; i < n; i++) {
        XiValue *v = blk->values[i];
        for (uint16_t a = 0; a < v->nargs; a++) {
            XiValue *arg = v->args[a];
            if (arg && arg->block == blk && arg->op != XI_PHI)
                indeg[i]++;
        }
        if (shuffle_is_effectful(blk, v)) {
            if (prev_effect >= 0)
                indeg[i]++;
            prev_effect = (int32_t) i;
        }
    }
}

/* For each value, record the index of its immediate effect-chain predecessor
 * in the original ordering, or -1 if none. */
static void shuffle_build_effect_pred(const XiBlock *blk, XiValue *const *values, uint32_t n,
                                      int32_t *effect_pred) {
    int32_t pe = -1;
    for (uint32_t i = 0; i < n; i++) {
        effect_pred[i] = -1;
        if (shuffle_is_effectful(blk, values[i])) {
            effect_pred[i] = pe;
            pe = (int32_t) i;
        }
    }
}

/* After value `idx` (== placed_v) has been emitted, decrement indeg of each
 * remaining value and push newly-ready indices onto `ready`. */
static void shuffle_release_dependents(const XiBlock *blk, XiValue *const *values, uint32_t n,
                                       uint32_t idx, XiValue *placed_v, const int32_t *effect_pred,
                                       uint32_t *indeg, uint32_t *ready, uint32_t *ready_count) {
    bool placed_is_effect = shuffle_is_effectful(blk, placed_v);
    bool placed_is_phi = placed_v->op == XI_PHI;
    for (uint32_t j = 0; j < n; j++) {
        if (indeg[j] == 0 || indeg[j] == UINT32_MAX)
            continue;
        XiValue *vj = values[j];
        uint32_t released = 0;
        if (!placed_is_phi) {
            for (uint16_t a = 0; a < vj->nargs; a++) {
                if (vj->args[a] == placed_v)
                    released++;
            }
        }
        if (placed_is_effect && effect_pred[j] == (int32_t) idx)
            released++;
        if (released > 0) {
            XR_DCHECK(indeg[j] >= released, "shuffle_block_values: indeg underflow");
            indeg[j] -= released;
            if (indeg[j] == 0)
                ready[(*ready_count)++] = j;
        }
    }
}

/* Randomized topological sort of values within a block.
 * Respects intra-block data dependencies (use after def) and the relative
 * order among memory-touching / effectful values.
 * Uses Kahn's algorithm with random selection from the ready set. */
static void shuffle_block_values(XiBlock *blk) {
    uint32_t n = blk->nvalues;
    if (n <= 1)
        return;

    /* Small stack buffer; fall back to heap for large blocks. */
    uint32_t stack_indeg[128], stack_ready[128];
    int32_t stack_epred[128];
    XiValue *stack_out[128];
    uint32_t *indeg = (n <= 128) ? stack_indeg : (uint32_t *) xr_malloc(n * sizeof(uint32_t));
    uint32_t *ready = (n <= 128) ? stack_ready : (uint32_t *) xr_malloc(n * sizeof(uint32_t));
    int32_t *effect_pred = (n <= 128) ? stack_epred : (int32_t *) xr_malloc(n * sizeof(int32_t));
    XiValue **out = (n <= 128) ? stack_out : (XiValue **) xr_malloc(n * sizeof(XiValue *));
    if (!indeg || !ready || !effect_pred || !out)
        goto cleanup;

    XiValue **result = blk->values;
    shuffle_count_indeg(blk, indeg);
    shuffle_build_effect_pred(blk, result, n, effect_pred);

    uint32_t ready_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (indeg[i] == 0)
            ready[ready_count++] = i;
    }

    uint32_t placed = 0;
    while (ready_count > 0) {
        uint32_t pick = (uint32_t) (rand() % ready_count);
        uint32_t idx = ready[pick];
        ready[pick] = ready[--ready_count];

        XiValue *placed_v = result[idx];
        out[placed++] = placed_v;
        indeg[idx] = UINT32_MAX; /* sentinel: already placed */

        shuffle_release_dependents(blk, result, n, idx, placed_v, effect_pred, indeg, ready,
                                   &ready_count);
    }

    /* A real cycle would be a Xi IR bug; skip silently in release. */
    if (placed == n) {
        memcpy(result, out, n * sizeof(XiValue *));
    }
#ifndef NDEBUG
    else {
        fprintf(stderr,
                "[xi_shuffle] warning: block b%u has %u values but "
                "only %u placed (possible dep cycle)\n",
                blk->id, n, placed);
    }
#endif

cleanup:
    if (n > 128) {
        if (indeg && indeg != stack_indeg)
            xr_free(indeg);
        if (ready && ready != stack_ready)
            xr_free(ready);
        if (effect_pred && effect_pred != stack_epred)
            xr_free(effect_pred);
        if (out && out != stack_out)
            xr_free(out);
    }
}

XR_FUNC XiOptResult xi_opt_run_pipeline_ex_with_mask(XiFunc *f, XiOptLevel level,
                                                     XiPipelineStats *stats, uint64_t budget_ns,
                                                     XiOptDisableMask disabled_passes) {
    XR_DCHECK(f != NULL, "xi_opt_run_pipeline_ex_with_mask: NULL func");

    XiOptResult result;
    memset(&result, 0, sizeof(result));
    result.ok = true;
    result.round = -1;

    validate_pass_table();

    XiAnalysisManager analysis_manager;
    xi_analysis_manager_init(&analysis_manager, f);

    if (level == XI_OPT_NONE) {
        result.change = xi_pass_no_change();
        return result;
    }

    if (stats)
        memset(stats, 0, sizeof(*stats));

    /* XRAY_XI_CHECK=1 enables per-pass verification to pinpoint
     * the exact pass that breaks an invariant. */
    xr_once_call(&xi_check_per_pass_once, init_xi_check_per_pass);

    /* XRAY_XI_DUMP=func:pass — dump IR after a specific pass for a
     * specific function.  Use "*" to match any func or pass name.
     * Examples: "main:dce", "*:constfold", "foo:*" */
    xr_once_call(&xi_dump_once, init_xi_dump_config);

    /* XRAY_XI_SHUFFLE=1: randomize block AND intra-block value iteration
     * order before each pass to detect implicit ordering dependencies.
     * Only active in debug builds. Values within a block are shuffled
     * using randomized topological sort that respects data dependencies
     * and side-effect ordering. */
    xr_once_call(&xi_shuffle_once, init_xi_shuffle_config);

    /* XRAY_XI_PASS=pass:key=value[,pass:key=value,...]
     * Per-pass control flags:
     *   enable=0  — skip this pass entirely
     *   dump=1    — dump IR after this pass (all funcs)
     * Examples: "dce:enable=0", "gvn:dump=1,licm:enable=0" */
    xr_once_call(&xi_pass_cfg_once, init_xi_pass_config);

    const XiOptDisableMask effective_disable = disabled_passes | xi_pass_env_disable_mask;

    uint64_t pipeline_start = xr_time_monotonic_ns();
    XiPassChange total = xi_pass_no_change();

    /* Fixed-point iteration: repeat until no pass reports a change */
    int round;
    for (round = 0; round < XI_OPT_MAX_ROUNDS; round++) {
        XiPassChange round_chg = xi_pass_no_change();

        for (size_t p = 0; p < XI_PASS_TABLE_SIZE; p++) {
            const XiPassDesc *desc = &xi_pass_table[p];
            if (desc->min_level > level)
                continue;

            /* Configuration and XRAY_XI_PASS both land in one mask. */
            if (pass_disabled_by_mask(desc, p, effective_disable))
                continue;
            /* Structural rewrites may delete or merge the frozen suspend,
             * resume, and dispatch anchors.  Until a pass publishes an audited
             * coroutine-plan preservation contract, do not enter it after
             * coroutine lowering.  Marked value rewrites still run and rebase
             * the plan below. */
            if (f->coro_plan && f->coro_plan->is_coroutine &&
                !(desc->flags & XI_PASS_CORO_PLAN_SAFE))
                continue;

            /* Budget check before each pass */
            if (budget_ns > 0) {
                uint64_t elapsed = xr_time_monotonic_ns() - pipeline_start;
                if (elapsed >= budget_ns)
                    goto done;
            }

            if (f->stage < desc->min_stage || f->stage > desc->max_stage) {
                result.ok = false;
                result.pass_name = desc->name;
                result.round = round;
                snprintf(result.detail, sizeof(result.detail),
                         "pass '%s' is illegal at stage %s (legal window %s..%s)", desc->name,
                         xi_stage_name(f->stage), xi_stage_name(desc->min_stage),
                         xi_stage_name(desc->max_stage));
                goto done;
            }

            XiInvariantMask missing_inv = desc->requires_inv_mask & ~f->invariant_mask;
            if (missing_inv) {
                result.ok = false;
                result.pass_name = desc->name;
                result.round = round;
                snprintf(result.detail, sizeof(result.detail),
                         "pass '%s' requires invariant bits 0x%x but func '%s' only has 0x%x",
                         desc->name, missing_inv, f->name ? f->name : "?", f->invariant_mask);
                goto done;
            }

            /* Shuffle blocks[1..n-1] (preserve entry at [0]) to catch
             * passes that assume RPO or insertion order.  Xi IR carries
             * an implicit invariant that block->id equals its index in
             * f->blocks[] (several passes — notably SCCP — index
             * per-block scratch by id and pass succ->id to mark_edge as
             * an array index).  After permuting the array we must
             * re-sync ids so the invariant holds, then bump cfg_version
             * so any cached RPO / dom / loop info recomputes. */
            if (xi_shuffle_blocks_enabled && f->nblocks > 2) {
                for (uint32_t si = f->nblocks - 1; si > 1; si--) {
                    uint32_t sj = 1 + (uint32_t) (rand() % si);
                    XiBlock *tmp = f->blocks[si];
                    f->blocks[si] = f->blocks[sj];
                    f->blocks[sj] = tmp;
                }
                for (uint32_t bi = 0; bi < f->nblocks; bi++) {
                    f->blocks[bi]->id = bi;
                }

                xi_cfg_invalidate(f);
                /* Shuffle values within each block (randomized topo sort
                 * respecting data deps and side-effect ordering). */
                for (uint32_t bi = 0; bi < f->nblocks; bi++) {
                    shuffle_block_values(f->blocks[bi]);
                }
                if (f->coro_plan && !xi_coro_plan_rebase(f)) {
                    result.ok = false;
                    result.pass_name = desc->name;
                    result.round = round;
                    snprintf(result.detail, sizeof(result.detail),
                             "debug shuffle could not preserve coroutine plan for '%s'",
                             f->name ? f->name : "?");
                    goto done;
                }
            }

            /* Debug shuffling changes CFG revision and value order. Acquire
             * proofs only after that instrumentation so a pass can never see
             * evidence stamped for the pre-shuffle graph. */
            if (desc->requires_evidence) {
                char evidence_error[256];
                if (!xi_analysis_require_proven_domains(&analysis_manager, desc->requires_evidence,
                                                        evidence_error, sizeof(evidence_error))) {
                    result.ok = false;
                    result.pass_name = desc->name;
                    result.round = round;
                    snprintf(result.detail, sizeof(result.detail),
                             "pass '%s' requires current evidence: %.180s", desc->name,
                             evidence_error);
                    goto done;
                }
            }

            uint64_t t0 = xr_time_monotonic_ns();
            XiEditSession edit_session;
            if (!xi_edit_begin(&edit_session, f)) {
                result.ok = false;
                result.pass_name = desc->name;
                result.round = round;
                snprintf(result.detail, sizeof(result.detail),
                         "pass '%s' could not open an Xi edit session", desc->name);
                goto done;
            }
            XiPassChange pc = desc->fn(f);

            XiPassOutcome pass_outcome;
            char edit_error[256] = {0};
            if (!xi_edit_finish(&edit_session, pc, desc->invalidates_evidence,
                                desc->preserves_evidence, &pass_outcome, edit_error,
                                sizeof(edit_error))) {
                result.ok = false;
                result.pass_name = desc->name;
                result.round = round;
                snprintf(result.detail, sizeof(result.detail),
                         "pass '%s' mutation audit failed: %.180s", desc->name, edit_error);
                goto done;
            }
            (void) pass_outcome;

            if (f->coro_plan &&
                (pass_outcome.revision_delta.ir_changed ||
                 pass_outcome.revision_delta.cfg_changed) &&
                !xi_coro_plan_rebase(f)) {
                result.ok = false;
                result.pass_name = desc->name;
                result.round = round;
                snprintf(result.detail, sizeof(result.detail),
                         "pass '%s' could not preserve coroutine plan for '%s'", desc->name,
                         f->name ? f->name : "?");
                goto done;
            }

            uint64_t dt = xr_time_monotonic_ns() - t0;

            XiInvariantMask missing_produced = desc->produces_inv_mask & ~f->invariant_mask;
            if (missing_produced) {
                result.ok = false;
                result.pass_name = desc->name;
                result.round = round;
                snprintf(result.detail, sizeof(result.detail),
                         "pass '%s' did not produce invariant bits 0x%x for func '%s'", desc->name,
                         missing_produced, f->name ? f->name : "?");
                goto done;
            }

            /* XRAY_XI_DUMP: targeted IR dump after matching pass */
            if (xi_dump_func && xi_dump_pass) {
                const char *fn = f->name ? f->name : "<anonymous>";
                bool func_match = (xi_dump_func[0] == '*' && xi_dump_func[1] == '\0') ||
                                  strcmp(xi_dump_func, fn) == 0;
                bool pass_match = (xi_dump_pass[0] == '*' && xi_dump_pass[1] == '\0') ||
                                  strcmp(xi_dump_pass, desc->name) == 0;
                if (func_match && pass_match) {
                    fprintf(stderr, "=== Xi IR after '%s' (func '%s', round %d) ===\n", desc->name,
                            fn, round);
                    xi_func_dump(f, stderr);
                    fprintf(stderr, "================================================\n");
                }
            }

            /* XRAY_XI_PASS per-pass dump (unconditional on function name) */
            if (xi_pass_cfg[p].dump) {
                const char *fn = f->name ? f->name : "<anonymous>";
                fprintf(stderr, "=== [XI_PASS dump] after '%s' (func '%s', round %d) ===\n",
                        desc->name, fn, round);
                xi_func_dump(f, stderr);
                fprintf(stderr, "=============================================\n");
            }

            /* Record per-pass stats */
            XiPassStats *ps = stats_slot(stats, desc->name);
            if (ps) {
                ps->invocations++;
                ps->n_removed += pc.n_removed;
                ps->n_added += pc.n_added;
                ps->elapsed_ns += dt;
            }

            round_chg = xi_pass_merge(round_chg, pc);

            for (uint32_t bit = 1; bit <= XI_EVD_MEMSSA; bit <<= 1u) {
                if ((desc->produces_evidence & bit) != 0 &&
                    !xi_evidence_domain_is_current(f, (XiEvidenceDomain) bit)) {
                    result.ok = false;
                    result.pass_name = desc->name;
                    result.round = round;
                    snprintf(result.detail, sizeof(result.detail),
                             "pass '%s' did not publish current '%s' evidence", desc->name,
                             xi_evidence_domain_name((XiEvidenceDomain) bit));
                    goto done;
                }
            }

            /* XRAY_XI_CHECK=1: verify after every single pass.
             * Uses stage-aware verification so stage-specific invariants
             * are also checked as the function progresses. */
            if (xi_check_per_pass_enabled) {
                char check_errbuf[512];
                if (!xi_verify_stage(f, f->stage, check_errbuf, sizeof(check_errbuf))) {
                    result.ok = false;
                    result.pass_name = desc->name;
                    result.round = round;
                    snprintf(result.detail, sizeof(result.detail),
                             "verify failed after pass '%s' round %d for '%s': %.360s", desc->name,
                             round, f->name ? f->name : "?", check_errbuf);
                    goto done;
                }
            }

            /* Task 219 P3: re-verify the RC contracts after any pass that
             * mutates lifetime/ownership evidence or the CFG. A lifetime-
             * invalidating optimization that reintroduces a use-after-release
             * or double-free then ICEs at the offending pass instead of
             * silently miscompiling. On by default in debug/CI, off in release
             * (see xi_arc_verify_per_pass_enabled); the post-ARC single run is
             * always on regardless. */
            if (xi_arc_verify_per_pass_enabled() &&
                ((desc->invalidates_evidence & (XI_EVD_LIFETIME | XI_EVD_OWNERSHIP)) != 0 ||
                 pc.cfg_changed)) {
                xi_arc_verify_or_ice(f, desc->name);
            }
        }

        total = xi_pass_merge(total, round_chg);

        /* This barrier is unconditional in every build.  An optimization
         * round that violates Xi contracts is a normal compiler-phase error,
         * not a reason to abort the process or feed damaged IR downstream. */
        {
            char errbuf[512];
            if (!xi_verify_stage(f, f->stage, errbuf, sizeof(errbuf))) {
                result.ok = false;
                result.round = round;
                snprintf(result.detail, sizeof(result.detail),
                         "verify failed after optimization round %d for '%s': %.380s", round,
                         f->name ? f->name : "?", errbuf);
                goto done;
            }
        }

        /* Converged: no pass changed anything this round */
        if (!round_chg.cfg_changed && !round_chg.values_changed && !round_chg.types_changed) {
            round++; /* count final round */
            break;
        }
    }

done:
    if (stats) {
        stats->total_rounds = (uint32_t) round;
        stats->total_ns = xr_time_monotonic_ns() - pipeline_start;
        stats->rpo_recomputes = f->rpo_recomputes;
        stats->dom_recomputes = f->dom_recomputes;
        stats->loop_recomputes = f->loop_recomputes;
    }

    result.change = total;
    if (!result.ok)
        return result;

    /* Recurse into nested functions / closures */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i]) {
            XiPipelineStats child_stats;
            XiPipelineStats *child_stats_ptr = stats ? &child_stats : NULL;
            XiOptResult child = xi_opt_run_pipeline_ex_with_mask(
                f->children[i], level, child_stats_ptr, budget_ns, disabled_passes);
            total = xi_pass_merge(total, child.change);
            if (stats)
                stats_merge(stats, &child_stats);
            if (!child.ok) {
                child.change = total;
                return child;
            }
        }
    }

    result.change = total;
    return result;
}

XR_FUNC XiOptResult xi_opt_run_pipeline_ex(XiFunc *f, XiOptLevel level, XiPipelineStats *stats,
                                           uint64_t budget_ns) {
    return xi_opt_run_pipeline_ex_with_mask(f, level, stats, budget_ns, XI_OPT_DISABLE_NONE);
}

XR_FUNC XiOptResult xi_opt_run_pipeline(XiFunc *f, XiOptLevel level) {
    return xi_opt_run_pipeline_ex(f, level, NULL, 0);
}

/* ========== Stats Dump ========== */

XR_FUNC void xi_pipeline_stats_dump(const XiPipelineStats *stats, const char *func_name) {
    if (!stats)
        return;
    fprintf(stderr, "[xi_stats] func '%s': %u rounds, %.3f ms total\n", func_name ? func_name : "?",
            stats->total_rounds, (double) stats->total_ns / 1e6);

    uint32_t total_invocations = 0;
    for (uint32_t i = 0; i < stats->npass; i++) {
        const XiPassStats *ps = &stats->passes[i];
        if (ps->invocations == 0)
            continue;
        fprintf(stderr, "  %-18s  %3u calls  %5u rem  %5u add  %7.3f ms\n", ps->name,
                ps->invocations, ps->n_removed, ps->n_added, (double) ps->elapsed_ns / 1e6);
        total_invocations += ps->invocations;
    }

    /* Cache effectiveness — recomputes vs total pass invocations.
     * Without caching, every pass that needs dominators would force a
     * full recompute; the ratio shows how much work xi_ensure_* saved. */
    fprintf(stderr, "  analysis-cache: rpo=%u dom=%u loop=%u  (across %u pass invocations)\n",
            stats->rpo_recomputes, stats->dom_recomputes, stats->loop_recomputes,
            total_invocations);
}
