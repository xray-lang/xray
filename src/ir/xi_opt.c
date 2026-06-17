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
#include "xi_opt_devirt.h"
#include "xi_opt_jump_thread.h"
#include "xi_opt_strength.h"
#include "xi_opt_tail_call.h"
#include "xi_opt_ivsr.h"
#include "xi_opt_loop_peel.h"
#include "xi_opt_loop_unroll.h"
#include "xi_opt_loop_split.h"
#include "xi_opt_loop_inv_branch.h"
#include "xi_block_layout.h"
#include "xi_opt_slp.h"
#include "xi_opt_loop_vec.h"
#include "xi_opt_reduction.h"
#include "xi_opt_call_specialize.h"
#include "xi_opt_comptime.h"
#include "xi_range.h"
#include "xi_analysis.h"
#include "xi_pass.h"
#include "xi_verify.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
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
            for (uint16_t a = 0; a < v->nargs; a++) {
                if (v->args[a] == old_val)
                    v->args[a] = new_val;
            }
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
static bool fold_int_binary(uint16_t op, int64_t a, int64_t b, int64_t *result) {
    switch (op) {
        case XI_ADD:
            *result = (int64_t) ((uint64_t) a + (uint64_t) b);
            return true;
        case XI_SUB:
            *result = (int64_t) ((uint64_t) a - (uint64_t) b);
            return true;
        case XI_MUL:
            *result = (int64_t) ((uint64_t) a * (uint64_t) b);
            return true;
        case XI_DIV:
            if (b == 0)
                return false;
            if (a == INT64_MIN && b == -1) {
                *result = INT64_MIN;
                return true;
            }
            *result = a / b;
            return true;
        case XI_MOD:
            if (b == 0)
                return false;
            if (a == INT64_MIN && b == -1) {
                *result = 0;
                return true;
            }
            *result = a % b;
            return true;
        case XI_BAND:
            *result = a & b;
            return true;
        case XI_BOR:
            *result = a | b;
            return true;
        case XI_BXOR:
            *result = a ^ b;
            return true;
        case XI_SHL:
            /* Left shift of a negative or shift that overflows the sign bit
             * is UB on signed; do it on uint64_t and cast back. Shift amount
             * is masked to 6 bits to match runtime semantics. */
            *result = (int64_t) ((uint64_t) a << (b & 63));
            return true;
        case XI_SHR:
            /* Arithmetic right shift on negative values is
             * implementation-defined in C99/C11 but well-defined on every
             * compiler xray supports (GCC, Clang, MSVC all sign-extend). */
            *result = a >> (b & 63);
            return true;
        default:
            return false;
    }
}

/* Try to fold a comparison on two integer constants. */
static bool fold_int_compare(uint16_t op, int64_t a, int64_t b, bool *result) {
    switch (op) {
        case XI_EQ:
            *result = (a == b);
            return true;
        case XI_NE:
            *result = (a != b);
            return true;
        case XI_LT:
            *result = (a < b);
            return true;
        case XI_LE:
            *result = (a <= b);
            return true;
        case XI_GT:
            *result = (a > b);
            return true;
        case XI_GE:
            *result = (a >= b);
            return true;
        default:
            return false;
    }
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
                if (fb == 0.0f)
                    return false;
                /* f32 division: narrowed operands, divide in double, narrow the
                 * quotient back to float (matches OP_DIV_F32 / AOT xrt_div). */
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
            if (b == 0.0)
                return false;
            *result = a / b;
            return true;
        default:
            return false;
    }
}

/* Try to fold a comparison on two float constants. */
static bool fold_float_compare(uint16_t op, double a, double b, bool *result) {
    switch (op) {
        case XI_EQ:
            *result = (a == b);
            return true;
        case XI_NE:
            *result = (a != b);
            return true;
        case XI_LT:
            *result = (a < b);
            return true;
        case XI_LE:
            *result = (a <= b);
            return true;
        case XI_GT:
            *result = (a > b);
            return true;
        case XI_GE:
            *result = (a >= b);
            return true;
        default:
            return false;
    }
}

static void rewrite_to_const_int(XiValue *v, int64_t value) {
    XR_DCHECK(v != NULL, "rewrite_to_const_int: NULL value");
    v->op = XI_CONST;
    v->aux_int = value;
    v->aux = NULL;
    v->nargs = 0;
    v->flags = xi_op_default_effects(XI_CONST);
    v->mem_group = XI_MEM_NONE;
}

static void rewrite_to_const_float(XiValue *v, double value) {
    XR_DCHECK(v != NULL, "rewrite_to_const_float: NULL value");
    v->op = XI_CONST;
    memcpy(&v->aux_int, &value, sizeof(double));
    v->aux = NULL;
    v->nargs = 0;
    v->flags = xi_op_default_effects(XI_CONST);
    v->mem_group = XI_MEM_NONE;
}

static void rewrite_to_copy(XiValue *v, XiValue *src) {
    XR_DCHECK(v != NULL, "rewrite_to_copy: NULL value");
    XR_DCHECK(src != NULL, "rewrite_to_copy: NULL source");
    v->op = XI_COPY;
    v->args[0] = src;
    v->nargs = 1;
    v->flags = xi_op_default_effects(XI_COPY);
    v->aux_int = 0;
    v->aux = NULL;
    v->mem_group = XI_MEM_NONE;
}

XR_FUNC XiPassChange xi_opt_const_fold(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_const_fold: NULL func");
    XiPassChange chg = xi_pass_no_change();

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];

            /* Fold unary NEG on const int.
             * -INT64_MIN is UB on signed; negate on uint64_t then cast back
             * to preserve wrap-on-overflow semantics (matches VM and AOT). */
            if (v->op == XI_NEG && v->nargs == 1 && is_const_int(v->args[0])) {
                rewrite_to_const_int(v, (int64_t) (0u - (uint64_t) v->args[0]->aux_int));
                chg.values_changed = true;
                continue;
            }

            /* Fold unary NEG on const float */
            if (v->op == XI_NEG && v->nargs == 1 && is_const_float(v->args[0])) {
                double val;
                memcpy(&val, &v->args[0]->aux_int, sizeof(double));
                val = -val;
                rewrite_to_const_float(v, val);
                chg.values_changed = true;
                continue;
            }

            /* Fold unary NOT on const bool */
            if (v->op == XI_NOT && v->nargs == 1 && is_const_bool(v->args[0])) {
                rewrite_to_const_int(v, v->args[0]->aux_int ? 0 : 1);
                chg.values_changed = true;
                continue;
            }

            /* Fold unary BNOT on const int */
            if (v->op == XI_BNOT && v->nargs == 1 && is_const_int(v->args[0])) {
                rewrite_to_const_int(v, ~(v->args[0]->aux_int));
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
            if (v->op == XI_TUPLE_GET && v->nargs == 1 && v->args[0] &&
                v->args[0]->op == XI_TUPLE_NEW) {
                XiValue *tup = v->args[0];
                int64_t idx = v->aux_int;
                if (idx >= 0 && (uint16_t) idx < tup->nargs && tup->args[(uint16_t) idx]) {
                    rewrite_to_copy(v, tup->args[(uint16_t) idx]);
                    /* nargs already 1; type stays as the projected element's type */
                    chg.values_changed = true;
                    continue;
                }
            }

            /* Binary: need exactly 2 args */
            if (v->nargs != 2)
                continue;
            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];

            /* Integer binary/compare */
            if (is_const_int(lhs) && is_const_int(rhs)) {
                int64_t result;
                if (fold_int_binary(v->op, lhs->aux_int, rhs->aux_int, &result)) {
                    rewrite_to_const_int(v, result);
                    chg.values_changed = true;
                    continue;
                }
                bool bres;
                if (fold_int_compare(v->op, lhs->aux_int, rhs->aux_int, &bres)) {
                    rewrite_to_const_int(v, bres ? 1 : 0);
                    chg.values_changed = true;
                    continue;
                }
            }

            /* Float binary/compare */
            if (is_const_float(lhs) && is_const_float(rhs)) {
                double a, b;
                memcpy(&a, &lhs->aux_int, sizeof(double));
                memcpy(&b, &rhs->aux_int, sizeof(double));

                double dresult;
                bool is_f32 = v->type && v->type->kind == XR_KIND_FLOAT &&
                              v->type->native_width == XR_NATIVE_F32;
                if (fold_float_binary(v->op, a, b, is_f32, &dresult)) {
                    rewrite_to_const_float(v, dresult);
                    chg.values_changed = true;
                    continue;
                }
                bool bres;
                if (fold_float_compare(v->op, a, b, &bres)) {
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
 * (e.g. `let temp = b`).  Resolving through it would merge the
 * domains, causing loop-carried variables to share a physical
 * register and corrupt each other on reassignment. */
static XiValue *resolve_copy(XiValue *v) {
    while (v && v->op == XI_COPY && v->nargs >= 1) {
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
        /* Stop at value-type copies — these become OP_COPY (deep copy) at
         * emit time and must not be propagated away */
        if (v->type && v->type->is_value_type)
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
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *resolved = resolve_copy(v->args[a]);
                if (resolved && resolved != v->args[a]) {
                    v->args[a] = resolved;
                    chg.values_changed = true;
                }
            }
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

static bool xi_identity_keeps_task_view(const XiValue *from, const XiValue *to) {
    if (!from || !to || (to->op != XI_COPY && to->op != XI_MOVE) || to->nargs != 1 ||
        to->args[0] != from)
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
            if (v->flags & XI_FLAG_FIRE_AND_FORGET)
                continue;
            if ((v->aux_int & 0xff) != 0)
                continue;

            XiValue *await = find_one_shot_await_for_go(f, v);
            if (!await || (await->aux_int & XI_AWAIT_AUX_ONE_SHOT_GO))
                continue;

            await->aux_int |= XI_AWAIT_AUX_ONE_SHOT_GO;
            v->aux_int |= XI_GO_AUX_ONE_SHOT_AWAIT;
            chg.values_changed = true;
            chg.n_added++;
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
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_TUPLE:
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
        default:
            return true;
    }
}

static bool sr_type_is_task_instance(const XrType *type) {
    if (!type)
        return false;
    if (type->kind == XR_KIND_INSTANCE)
        return type->instance.class_name && strcmp(type->instance.class_name, "Task") == 0;
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
    return method && (strcmp(method, "recv") == 0 || strcmp(method, "tryRecv") == 0);
}

static bool sr_field_receiver_uses_native_rep(const XiValue *receiver) {
    if (!receiver || !receiver->type || receiver->type->kind != XR_KIND_INSTANCE)
        return false;
    return !sr_type_is_task_instance(receiver->type);
}

static const XrType *sr_array_elem_type(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ARRAY)
        return type->container.element_type;
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return type->fixed_array.element_type;
    return NULL;
}

static XrRep sr_typed_array_elem_rep(const XrType *type) {
    const XrType *elem = sr_array_elem_type(type);
    if (!elem || elem->is_nullable)
        return XR_REP_TAGGED;
    switch (elem->native_width) {
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

static const XiValue *sr_unwrap_identity_value(const XiValue *v) {
    while (v && (v->op == XI_BOX || v->op == XI_UNBOX || v->op == XI_COPY || v->op == XI_MOVE) &&
           v->nargs >= 1)
        v = v->args[0];
    return v;
}

static bool sr_value_has_static_typed_array_storage_depth(const XiValue *value, uint8_t depth) {
    const XiValue *v = sr_unwrap_identity_value(value);
    if (!v || depth > 8 || sr_typed_array_elem_rep(v->type) == XR_REP_TAGGED)
        return false;
    if (v->op == XI_ARRAY_NEW)
        return true;
    if (v->op == XI_LOAD_FIELD)
        return true;
    if (v->op == XI_SLICE && v->nargs >= 1)
        return sr_value_has_static_typed_array_storage_depth(v->args[0], depth + 1);
    if (v->op == XI_CALL_BUILTIN) {
        const char *name = (const char *) v->aux;
        if (name && (strcmp(name, "array_new") == 0 || strcmp(name, "Bytes") == 0))
            return true;
        if (name && strcmp(name, "slice") == 0 && v->nargs >= 1)
            return sr_value_has_static_typed_array_storage_depth(v->args[0], depth + 1);
        return false;
    }
    if (v->op == XI_CALL_METHOD && v->nargs >= 1) {
        const char *method = (const char *) v->aux;
        if (method && strcmp(method, "slice") == 0)
            return sr_value_has_static_typed_array_storage_depth(v->args[0], depth + 1);
        if (method && strcmp(method, "filter") == 0)
            return sr_value_has_static_typed_array_storage_depth(v->args[0], depth + 1);
        if (method && strcmp(method, "map") == 0)
            return true;
    }
    if (v->op == XI_PHI) {
        bool has_base = false;
        if (v->nargs == 0)
            return false;
        for (uint16_t i = 0; i < v->nargs; i++) {
            const XiValue *arg = sr_unwrap_identity_value(v->args[i]);
            if (arg == v)
                continue;
            if (!sr_value_has_static_typed_array_storage_depth(arg, depth + 1))
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
    if (!v || v->op != XI_STRUCT_GET || v->nargs < 1)
        return false;
    const XrStructLayout *sl = (const XrStructLayout *) v->aux;
    if (!sl || v->aux_int < 0 || v->aux_int >= sl->field_count)
        return false;
    return sl->fields[v->aux_int].native_type == XR_NATIVE_ARRAY;
}

static bool sr_value_is_typed_array_field_ref(const XiValue *value) {
    const XiValue *v = sr_unwrap_identity_value(value);
    if (!v || v->op != XI_STRUCT_GET || v->nargs < 1)
        return false;
    const XrStructLayout *sl = (const XrStructLayout *) v->aux;
    if (!sl || v->aux_int < 0 || v->aux_int >= sl->field_count)
        return false;
    return sl->fields[v->aux_int].native_type == XR_NATIVE_ARRAY_REF &&
           sr_typed_array_elem_rep(v->type) != XR_REP_TAGGED;
}

static bool sr_value_has_static_index_storage(const XiValue *value) {
    return sr_value_has_static_typed_array_storage(value) ||
           sr_value_is_typed_array_field_ref(value) || sr_value_is_fixed_array_field_ref(value);
}

static bool sr_value_has_static_unboxed_array_elem_type(const XiValue *value) {
    const XiValue *v = sr_unwrap_identity_value(value);
    return v && sr_typed_array_elem_rep(v->type) != XR_REP_TAGGED;
}

static bool sr_is_typed_array_length_field(const XiValue *v) {
    if (!v || v->nargs < 1 || !v->args[0])
        return false;
    if (!sr_value_has_static_typed_array_storage(v->args[0]) &&
        !sr_value_is_typed_array_field_ref(v->args[0]))
        return false;
    const char *field = (const char *) v->aux;
    return field && (strcmp(field, "length") == 0 || strcmp(field, "size") == 0);
}

static bool sr_is_static_collection_length_field(const XiValue *v) {
    if (!v || v->nargs < 1 || !v->args[0] || !v->aux)
        return false;
    const char *field = (const char *) v->aux;
    if (strcmp(field, "length") != 0 && strcmp(field, "size") != 0)
        return false;
    const XiValue *receiver = sr_unwrap_identity_value(v->args[0]);
    if (!receiver || !receiver->type)
        return false;
    return receiver->type->kind == XR_KIND_ARRAY || receiver->type->kind == XR_KIND_MAP ||
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
    if (strcmp(name, "has") == 0)
        return symbol == SYMBOL_HAS;
    if (strcmp(name, "get") == 0)
        return symbol == SYMBOL_GET;
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
        case XI_STRUCT_GET:
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
            return a->nargs >= 1 && b->nargs >= 1 &&
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
    if (!has || has->op != XI_CALL_METHOD || has->nargs != 2 || !sr_method_name_is(has, "has"))
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

static XrRep sr_def_rep(const XiValue *v, const XiRepPolicy *policy) {
    if (!v || !v->type)
        return XR_REP_TAGGED;
    switch (v->op) {
        case XI_PARAM: {
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
        case XI_SHR: {
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
        case XI_CALL_METHOD_DIRECT:
            return sr_type_native_boundary_rep(v->type);
        case XI_CALL_METHOD:
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
        case XI_STRUCT_GET:
            return sr_type_scalar_rep(v->type);
        case XI_INDEX_GET:
            if (v->nargs >= 1 && v->args[0] && sr_value_has_static_index_storage(v->args[0]))
                return sr_typed_array_elem_rep(v->args[0]->type);
            return XR_REP_TAGGED;
        case XI_BYTES_LOAD_U32_LE:
        case XI_BYTES_LOAD_U64_LE:
            return XR_REP_I64;
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
static XrRep sr_use_rep(const XiValue *user, uint16_t arg_idx, const XiRepPolicy *policy) {
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
        case XI_SHR: {
            return sr_type_scalar_rep(user->type);
        }
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_EQ_STRICT:
        case XI_NE_STRICT:
            if (sr_compare_uses_null(user))
                return XR_REP_TAGGED;
            if (arg_idx < user->nargs && user->args[arg_idx] && user->args[arg_idx]->type) {
                return sr_type_scalar_rep(user->args[arg_idx]->type);
            }
            return XR_REP_TAGGED;
        case XI_NOT:
            if (arg_idx == 0 && user->args[0])
                return sr_def_rep(user->args[0], policy);
            return XR_REP_TAGGED;
        case XI_SELECT:
            if (arg_idx == 0 && user->args[0])
                return sr_def_rep(user->args[0], policy);
            if (arg_idx < 3) {
                return sr_def_rep(user, policy);
            }
            return XR_REP_TAGGED;
        case XI_CALL:
            if (arg_idx > 0 && policy && policy->prefer_call_args_native && arg_idx < user->nargs &&
                user->args[arg_idx]) {
                return sr_type_native_boundary_rep(user->args[arg_idx]->type);
            }
            return XR_REP_TAGGED;
        case XI_CALL_BUILTIN: {
            const char *name = (const char *) user->aux;
            if (name && strcmp(name, "Bytes") == 0 && arg_idx < user->nargs &&
                user->args[arg_idx] && user->args[arg_idx]->type &&
                user->args[arg_idx]->type->kind == XR_KIND_INT)
                return XR_REP_I64;
            return XR_REP_TAGGED;
        }
        case XI_CALL_METHOD:
        case XI_CALL_METHOD_DIRECT:
            if (arg_idx > 0 && policy && policy->prefer_call_args_native && arg_idx < user->nargs &&
                user->args[arg_idx]) {
                return sr_type_native_boundary_rep(user->args[arg_idx]->type);
            }
            return XR_REP_TAGGED;
        case XI_INDEX_GET:
            if (user->nargs >= 2 && user->args[0] &&
                sr_value_has_static_index_storage(user->args[0])) {
                if (arg_idx == 0)
                    return sr_type_native_boundary_rep(user->args[0]->type);
                if (arg_idx == 1)
                    return XR_REP_I64;
            }
            return XR_REP_TAGGED;
        case XI_INDEX_SET:
            if (user->nargs >= 3 && user->args[0] &&
                (sr_value_has_static_index_storage(user->args[0]) ||
                 sr_value_has_static_unboxed_array_elem_type(user->args[0]))) {
                if (arg_idx == 0)
                    return sr_type_native_boundary_rep(user->args[0]->type);
                if (arg_idx == 1)
                    return XR_REP_I64;
                if (arg_idx == 2)
                    return sr_typed_array_elem_rep(user->args[0]->type);
            }
            return XR_REP_TAGGED;
        case XI_BYTES_LOAD_U32_LE:
        case XI_BYTES_LOAD_U64_LE:
            return arg_idx == 1 ? XR_REP_I64 : XR_REP_TAGGED;
        case XI_BYTES_COPY_WITHIN:
        case XI_BYTES_REPEAT_FROM:
            return arg_idx == 0 ? XR_REP_TAGGED : XR_REP_I64;
        case XI_BYTES_COPY_FROM:
            return arg_idx <= 1 ? XR_REP_TAGGED : XR_REP_I64;
        case XI_STRUCT_SET:
            if (arg_idx == 1 && user->nargs >= 2 && user->args[1])
                return sr_type_scalar_rep(user->args[1]->type);
            return XR_REP_TAGGED;
        case XI_LOAD_FIELD:
            if (arg_idx == 0 && user->nargs >= 1 &&
                (sr_is_typed_array_length_field(user) ||
                 sr_is_static_collection_length_field(user)))
                return sr_type_native_boundary_rep(user->args[0]->type);
            return XR_REP_TAGGED;
        case XI_STORE_FIELD:
            if (arg_idx == 1 && policy && policy->prefer_call_args_native && user->nargs >= 2 &&
                sr_field_receiver_uses_native_rep(user->args[0]) && user->args[1])
                return sr_type_scalar_rep(user->args[1]->type);
            return XR_REP_TAGGED;
        case XI_ARRAY_NEW:
        case XI_MAP_NEW:
        case XI_SET_NEW:
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
static XiValue *sr_make_convert(XiFunc *f, XiBlock *blk, uint16_t op, struct XrType *type,
                                XiValue *arg) {
    XR_DCHECK(f != NULL, "sr_make_convert: NULL func");
    XR_DCHECK(blk != NULL, "sr_make_convert: NULL block");
    XR_DCHECK(arg != NULL, "sr_make_convert: NULL arg");
    XiValue *v = (XiValue *) xi_func_arena_alloc(f, sizeof(XiValue));
    if (!v)
        return NULL;
    memset(v, 0, sizeof(XiValue));
    v->id = f->next_value_id++;
    v->op = op;
    v->var_id = arg->var_id;
    v->type = type;
    v->nargs = 1;
    v->uses = -1;
    v->block = blk;
    v->args = (XiValue **) xi_func_arena_alloc(f, sizeof(XiValue *));
    if (!v->args)
        return NULL;
    v->args[0] = arg;
    return v;
}

/* Rewrite a single arg reference if rep mismatches. */
static void sr_rewrite_arg(XiFunc *f, XiValue **arg_slot, XrRep use_r, XiValue **box_of,
                           XiValue **unbox_of, uint32_t max_id, const XiRepPolicy *policy) {
    XiValue *arg = *arg_slot;
    if (!arg || arg->id >= max_id)
        return;
    XrRep def_r = sr_def_rep(arg, policy);
    if (def_r == use_r)
        return;

    if (def_r != XR_REP_TAGGED && use_r == XR_REP_TAGGED) {
        /* Unboxed -> tagged: insert BOX */
        if (!box_of[arg->id]) {
            box_of[arg->id] = sr_make_convert(f, arg->block, XI_BOX, arg->type, arg);
        }
        if (box_of[arg->id])
            *arg_slot = box_of[arg->id];
    } else if (def_r == XR_REP_TAGGED && use_r != XR_REP_TAGGED) {
        /* Tagged -> unboxed: insert UNBOX */
        if (!unbox_of[arg->id]) {
            unbox_of[arg->id] = sr_make_convert(f, arg->block, XI_UNBOX, arg->type, arg);
        }
        if (unbox_of[arg->id])
            *arg_slot = unbox_of[arg->id];
    }
}

XR_FUNC XiPassChange xi_opt_select_rep_with_policy(XiFunc *f, const XiRepPolicy *policy) {
    XR_DCHECK(f != NULL, "xi_opt_select_rep_with_policy: NULL func");
    XiRepPolicy local_policy = policy ? *policy : xi_rep_policy_tagged_boundary();

    uint32_t max_id = f->next_value_id;
    if (max_id == 0)
        return xi_pass_no_change();

    XiValue **box_of = (XiValue **) xr_calloc(max_id, sizeof(XiValue *));
    XiValue **unbox_of = (XiValue **) xr_calloc(max_id, sizeof(XiValue *));
    if (!box_of || !unbox_of) {
        xr_free(box_of);
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
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                XrRep use_r = sr_use_rep(v, ai, &local_policy);
                sr_rewrite_arg(f, &v->args[ai], use_r, box_of, unbox_of, max_id, &local_policy);
            }
        }

        /* Phi args follow the selected backend policy.  VM-style consumers keep
         * merge points tagged; AOT can keep native boundary phis unboxed. */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            XrRep phi_rep = local_policy.force_phi_tagged
                                ? XR_REP_TAGGED
                                : sr_type_native_boundary_rep(phi->value.type);
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                sr_rewrite_arg(f, &phi->value.args[ai], phi_rep, box_of, unbox_of, max_id,
                               &local_policy);
            }
        }

        /* Return control follows the function ABI policy. */
        if (blk->kind == XI_BLOCK_RETURN && blk->control) {
            XrRep ret_rep = local_policy.force_return_tagged
                                ? XR_REP_TAGGED
                                : sr_type_native_boundary_rep(f->return_type);
            sr_rewrite_arg(f, &blk->control, ret_rep, box_of, unbox_of, max_id, &local_policy);
        }
    }

    /* Rebuild each block's value array to include BOX/UNBOX after source.
     * PHI nodes live on blk->phis, not in values[], so we must also
     * check phi-sourced BOX/UNBOX and prepend them to the block. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        uint32_t extra = 0;
        /* Count conversions sourced from regular values */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v)
                continue;
            if (v->id < max_id && box_of[v->id] && box_of[v->id]->block == blk)
                extra++;
            if (v->id < max_id && unbox_of[v->id] && unbox_of[v->id]->block == blk)
                extra++;
        }
        /* Count conversions sourced from phi nodes */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t pid = phi->value.id;
            if (pid < max_id && box_of[pid] && box_of[pid]->block == blk)
                extra++;
            if (pid < max_id && unbox_of[pid] && unbox_of[pid]->block == blk)
                extra++;
        }
        if (extra == 0)
            continue;

        uint32_t new_cap = blk->nvalues + extra;
        XiValue **nv = (XiValue **) xi_func_arena_alloc(f, new_cap * sizeof(XiValue *));
        if (!nv)
            continue;

        uint32_t ni = 0;
        /* Prepend phi-sourced BOX/UNBOX before regular instructions */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            uint32_t pid = phi->value.id;
            if (pid < max_id && unbox_of[pid] && unbox_of[pid]->block == blk)
                nv[ni++] = unbox_of[pid];
            if (pid < max_id && box_of[pid] && box_of[pid]->block == blk)
                nv[ni++] = box_of[pid];
        }
        /* Then regular values with their conversions */
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            nv[ni++] = v;
            if (!v || v->id >= max_id)
                continue;
            if (unbox_of[v->id] && unbox_of[v->id]->block == blk)
                nv[ni++] = unbox_of[v->id];
            if (box_of[v->id] && box_of[v->id]->block == blk)
                nv[ni++] = box_of[v->id];
        }
        blk->values = nv;
        blk->nvalues = ni;
        blk->values_cap = new_cap;
    }

    xr_free(box_of);
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

    f->stage = XI_STAGE_REPPED;
    f->invariant_mask |= xi_stage_invariants(XI_STAGE_REPPED);
    return xi_pass_change_all();
}

XR_FUNC XiPassChange xi_opt_select_rep(XiFunc *f) {
    XiRepPolicy policy = xi_rep_policy_tagged_boundary();
    return xi_opt_select_rep_with_policy(f, &policy);
}

/* ========== BOX/UNBOX Peephole Elimination ========== */

/*
 * Eliminate inverse BOX/UNBOX pairs:
 *   UNBOX(BOX(x)) -> COPY(x)
 *   BOX(UNBOX(x)) -> COPY(x)
 * Subsequent copy-prop and DCE clean up the COPY and dead BOX/UNBOX.
 */
XR_FUNC XiPassChange xi_opt_box_elim(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_box_elim: NULL func");
    XiPassChange chg = xi_pass_no_change();

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->nargs < 1 || !v->args[0])
                continue;

            XiValue *inner = v->args[0];
            if (inner->nargs < 1 || !inner->args[0])
                continue;

            bool elim = (v->op == XI_UNBOX && inner->op == XI_BOX) ||
                        (v->op == XI_BOX && inner->op == XI_UNBOX);
            if (elim) {
                rewrite_to_copy(v, inner->args[0]);
                chg.values_changed = true;
            }
        }
    }

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_opt_box_elim(f->children[i]);
    }
    return chg;
}

/* ========== Combined Pass ========== */

XR_FUNC void xi_opt_run(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_run: NULL func");
    xi_opt_run_pipeline(f, XI_OPT_LIGHT);
}

/* ========== Pipeline Driver ========== */

/* Pass table: ordered by recommended execution sequence.
 * The driver runs all passes whose min_level <= requested level. */
static const XiPassDesc xi_pass_table[] = {
    /* name              fn                       min_level      flags               in_stage
       out_stage       requires              produces */
    {"tbaa", xi_tbaa_annotate, XI_OPT_LIGHT, XI_PASS_REQUIRED, XI_STAGE_RAW, XI_STAGE_RAW, 0,
     XI_INV_TBAA_ANNOTATED},
    {"constfold", xi_opt_const_fold, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"strength_reduce", xi_opt_strength_reduce, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW,
     XI_STAGE_RAW, 0, 0},
    {"copy_prop", xi_opt_copy_prop, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"mark_one_shot_await", xi_opt_mark_one_shot_await, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW,
     XI_STAGE_RAW, 0, 0},
    {"phi_simplify", xi_opt_phi_simplify, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0,
     0},
    {"dce", xi_opt_dce, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"devirt", xi_opt_devirt, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"sccp", xi_opt_sccp, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"range", xi_range_analyze, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0,
     XI_INV_RANGE_ANNOTATED},
    {"bce", xi_opt_bce, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW,
     XI_INV_RANGE_ANNOTATED, 0},
    {"gvn", xi_opt_gvn_pre, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW,
     XI_INV_TBAA_ANNOTATED, 0},
    {"loop_rotate", xi_opt_loop_rotate, XI_OPT_FULL, XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP,
     XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"licm", xi_opt_licm, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW,
     XI_INV_TBAA_ANNOTATED, 0},
    {"ivsr", xi_opt_ivsr, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"loop_peel", xi_opt_loop_peel, XI_OPT_FULL, XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP,
     XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"loop_unroll", xi_opt_loop_unroll, XI_OPT_FULL, XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP,
     XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"loop_split", xi_opt_loop_split, XI_OPT_FULL, XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP,
     XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"loop_inv_branch", xi_opt_loop_inv_branch, XI_OPT_FULL, XI_PASS_NEEDS_DOM | XI_PASS_NEEDS_LOOP,
     XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"inline", xi_opt_inline, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"tail_call", xi_opt_tail_call, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"ifconv", xi_opt_ifconv, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"jump_thread", xi_opt_jump_thread, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW,
     0, 0},
    {"block_simplify", xi_opt_block_simplify, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW,
     0, 0},
    {"block_layout", xi_opt_block_layout, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW,
     XI_STAGE_RAW, 0, 0},
    {"slp", xi_opt_slp, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW, 0, 0},
    {"loop_vec", xi_opt_loop_vec, XI_OPT_FULL, XI_PASS_NEEDS_LOOP, XI_STAGE_RAW, XI_STAGE_RAW, 0,
     0},
    {"reduction", xi_opt_reduction, XI_OPT_FULL, XI_PASS_NEEDS_LOOP, XI_STAGE_RAW, XI_STAGE_RAW, 0,
     0},
    {"call_specialize", xi_opt_call_specialize, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW,
     XI_STAGE_RAW, 0, 0},
    {"comptime_eval", xi_opt_comptime_eval, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW,
     0, 0},
};

#define XI_PASS_TABLE_SIZE (sizeof(xi_pass_table) / sizeof(xi_pass_table[0]))

/* Validate pass table invariants at startup.  Called once. */
static void validate_pass_table(void) {
    static bool validated = false;
    if (validated)
        return;
    validated = true;

    for (size_t i = 0; i < XI_PASS_TABLE_SIZE; i++) {
        const XiPassDesc *d = &xi_pass_table[i];
        XR_DCHECK(d->name != NULL, "pass table entry has NULL name");
        XR_DCHECK(d->fn != NULL, "pass table entry has NULL fn");
        /* output_stage must be >= input_stage (stages never go backwards) */
        XR_DCHECK(d->output_stage >= d->input_stage, "pass has output_stage < input_stage");
    }

    /* Verify stage monotonicity across the table: no pass should require
     * a higher input_stage than any earlier pass's output_stage can reach. */
    XiStage max_output = XI_STAGE_RAW;
    for (size_t i = 0; i < XI_PASS_TABLE_SIZE; i++) {
        const XiPassDesc *d = &xi_pass_table[i];
        if (d->input_stage > max_output) {
            /* This pass requires a stage that no earlier pass produces.
             * This is allowed only if an external stage-transition pass
             * (not in this table) runs between them.  Log a diagnostic
             * in debug builds but don't abort — the pipeline driver
             * will soft-skip it via the stage check. */
#ifndef NDEBUG
            fprintf(stderr,
                    "[xi_pass] warning: pass '%s' requires stage %s "
                    "but max reachable from earlier passes is %s\n",
                    d->name, xi_stage_name(d->input_stage), xi_stage_name(max_output));
#endif
        }
        if (d->output_stage > max_output)
            max_output = d->output_stage;
    }

    /* Check declarative pass ordering constraints */
    XR_CHECK(xi_pass_order_check(), "xi pass order check failed");
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

static bool pass_disabled_by_mask(const XiPassDesc *desc, XiOptDisableMask disabled_passes) {
    if (!desc || (desc->flags & XI_PASS_REQUIRED))
        return false;
    if ((disabled_passes & XI_OPT_DISABLE_IVSR) && strcmp(desc->name, "ivsr") == 0)
        return true;
    return false;
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

XR_FUNC XiPassChange xi_opt_run_pipeline_ex_with_mask(XiFunc *f, XiOptLevel level,
                                                      XiPipelineStats *stats, uint64_t budget_ns,
                                                      XiOptDisableMask disabled_passes) {
    XR_DCHECK(f != NULL, "xi_opt_run_pipeline_ex_with_mask: NULL func");

    validate_pass_table();

    if (level == XI_OPT_NONE)
        return xi_pass_no_change();

    if (stats)
        memset(stats, 0, sizeof(*stats));

    /* XRAY_XI_CHECK=1 enables per-pass verification to pinpoint
     * the exact pass that breaks an invariant. */
    static int check_per_pass = -1;
    if (check_per_pass < 0) {
        const char *env = getenv("XRAY_XI_CHECK");
        check_per_pass = (env && env[0] == '1') ? 1 : 0;
    }

    /* XRAY_XI_DUMP=func:pass — dump IR after a specific pass for a
     * specific function.  Use "*" to match any func or pass name.
     * Examples: "main:dce", "*:constfold", "foo:*" */
    static const char *dump_func = NULL;
    static const char *dump_pass = NULL;
    static bool dump_parsed = false;
    if (!dump_parsed) {
        dump_parsed = true;
        const char *dump_env = getenv("XRAY_XI_DUMP");
        if (dump_env && dump_env[0]) {
            /* Find the colon separator */
            const char *colon = strchr(dump_env, ':');
            if (colon) {
                static char dump_func_buf[64];
                static char dump_pass_buf[64];
                size_t flen = (size_t) (colon - dump_env);
                if (flen >= sizeof(dump_func_buf))
                    flen = sizeof(dump_func_buf) - 1;
                memcpy(dump_func_buf, dump_env, flen);
                dump_func_buf[flen] = '\0';
                dump_func = dump_func_buf;
                strncpy(dump_pass_buf, colon + 1, sizeof(dump_pass_buf) - 1);
                dump_pass_buf[sizeof(dump_pass_buf) - 1] = '\0';
                dump_pass = dump_pass_buf;
            }
        }
    }

    /* XRAY_XI_SHUFFLE=1: randomize block AND intra-block value iteration
     * order before each pass to detect implicit ordering dependencies.
     * Only active in debug builds. Values within a block are shuffled
     * using randomized topological sort that respects data dependencies
     * and side-effect ordering. */
    static int shuffle_blocks = -1;
    if (shuffle_blocks < 0) {
        const char *env = getenv("XRAY_XI_SHUFFLE");
        shuffle_blocks = (env && env[0] == '1') ? 1 : 0;
        if (shuffle_blocks) {
            /* XRAY_XI_SHUFFLE_SEED=N — deterministic shuffle for repro.
             * Defaults to time-based seed when unset / 0. */
            const char *seed_env = getenv("XRAY_XI_SHUFFLE_SEED");
            unsigned seed = (seed_env && seed_env[0]) ? (unsigned) strtoul(seed_env, NULL, 10) : 0;
            if (seed == 0)
                seed = (unsigned) xr_time_monotonic_ns();
            srand(seed);
        }
    }

    /* XRAY_XI_PASS=pass:key=value[,pass:key=value,...]
     * Per-pass control flags:
     *   enable=0  — skip this pass entirely
     *   dump=1    — dump IR after this pass (all funcs)
     * Examples: "dce:enable=0", "gvn:dump=1,licm:enable=0" */
    static bool pass_cfg_parsed = false;
    static struct {
        bool disable;
        bool dump;
    } pass_cfg[XI_PASS_TABLE_SIZE];
    if (!pass_cfg_parsed) {
        pass_cfg_parsed = true;
        memset(pass_cfg, 0, sizeof(pass_cfg));
        const char *env = getenv("XRAY_XI_PASS");
        if (env && env[0]) {
            char buf[256];
            strncpy(buf, env, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            char *tok = strtok(buf, ",");
            while (tok) {
                char *colon = strchr(tok, ':');
                if (colon) {
                    *colon = '\0';
                    const char *pname = tok;
                    const char *kv = colon + 1;
                    int idx = pass_index_by_name(pname);
                    if (idx >= 0) {
                        if (strncmp(kv, "enable=0", 8) == 0)
                            pass_cfg[idx].disable = true;
                        else if (strncmp(kv, "dump=1", 6) == 0)
                            pass_cfg[idx].dump = true;
                    } else {
                        fprintf(stderr,
                                "[xi_pass] warning: unknown pass '%s' "
                                "in XRAY_XI_PASS\n",
                                pname);
                    }
                }
                tok = strtok(NULL, ",");
            }
        }
    }

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

            /* XRAY_XI_PASS: skip disabled passes (unless required) */
            if (pass_cfg[p].disable && !(desc->flags & XI_PASS_REQUIRED))
                continue;
            if (pass_disabled_by_mask(desc, disabled_passes))
                continue;

            /* Budget check before each pass */
            if (budget_ns > 0) {
                uint64_t elapsed = xr_time_monotonic_ns() - pipeline_start;
                if (elapsed >= budget_ns)
                    goto done;
            }

            /* Stage contract: skip pass if function has not reached
             * the required stage.  This is a soft check — the pass
             * simply does not fire rather than aborting. */
            if (desc->input_stage > f->stage)
                continue;

            XiInvariantMask missing_inv = desc->requires_inv_mask & ~f->invariant_mask;
            if (missing_inv) {
                fprintf(stderr,
                        "[xi_pass] pass '%s' requires invariant bits 0x%x "
                        "but func '%s' only has 0x%x\n",
                        desc->name, missing_inv, f->name ? f->name : "?", f->invariant_mask);
                XR_CHECK(false, "xi pass missing required invariant");
            }

            /* Shuffle blocks[1..n-1] (preserve entry at [0]) to catch
             * passes that assume RPO or insertion order.  Xi IR carries
             * an implicit invariant that block->id equals its index in
             * f->blocks[] (several passes — notably SCCP — index
             * per-block scratch by id and pass succ->id to mark_edge as
             * an array index).  After permuting the array we must
             * re-sync ids so the invariant holds, then bump cfg_version
             * so any cached RPO / dom / loop info recomputes. */
            if (shuffle_blocks && f->nblocks > 2) {
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
            }

            uint64_t t0 = xr_time_monotonic_ns();
            XiPassChange pc = desc->fn(f);

            /* Advance stage if the pass declares a higher output stage */
            if (desc->output_stage > f->stage) {
                f->stage = desc->output_stage;
                f->invariant_mask |= xi_stage_invariants(f->stage);
            }
            uint64_t dt = xr_time_monotonic_ns() - t0;

            XiInvariantMask missing_produced = desc->produces_inv_mask & ~f->invariant_mask;
            if (missing_produced) {
                fprintf(stderr,
                        "[xi_pass] pass '%s' did not produce invariant bits "
                        "0x%x for func '%s'\n",
                        desc->name, missing_produced, f->name ? f->name : "?");
                XR_CHECK(false, "xi pass invariant production failed");
            }

            /* XRAY_XI_DUMP: targeted IR dump after matching pass */
            if (dump_func && dump_pass) {
                const char *fn = f->name ? f->name : "<anonymous>";
                bool func_match =
                    (dump_func[0] == '*' && dump_func[1] == '\0') || strcmp(dump_func, fn) == 0;
                bool pass_match = (dump_pass[0] == '*' && dump_pass[1] == '\0') ||
                                  strcmp(dump_pass, desc->name) == 0;
                if (func_match && pass_match) {
                    fprintf(stderr, "=== Xi IR after '%s' (func '%s', round %d) ===\n", desc->name,
                            fn, round);
                    xi_func_dump(f, stderr);
                    fprintf(stderr, "================================================\n");
                }
            }

            /* XRAY_XI_PASS per-pass dump (unconditional on function name) */
            if (pass_cfg[p].dump) {
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

            /* If the pass changed the CFG, invalidate cached RPO /
             * dominators so the next xi_ensure_*() recomputes. */
            if (pc.cfg_changed)
                xi_cfg_invalidate(f);

            /* XRAY_XI_CHECK=1: verify after every single pass.
             * Uses stage-aware verification so stage-specific invariants
             * are also checked as the function progresses. */
            if (check_per_pass) {
                char check_errbuf[512];
                if (!xi_verify_stage(f, f->stage, check_errbuf, sizeof(check_errbuf))) {
                    fprintf(stderr,
                            "[xi_check] verify failed after pass '%s' "
                            "round %d for '%s': %s\n",
                            desc->name, round, f->name ? f->name : "?", check_errbuf);
                    XR_DCHECK(false, "XRAY_XI_CHECK: post-pass verify failed");
                }
            }
        }

        total = xi_pass_merge(total, round_chg);

        /* Converged: no pass changed anything this round */
        if (!round_chg.cfg_changed && !round_chg.values_changed && !round_chg.types_changed) {
            round++; /* count final round */
            break;
        }

#ifndef NDEBUG
        /* Re-verify after each round in debug builds */
        if (!check_per_pass) {
            char errbuf[512];
            if (!xi_verify(f, errbuf, sizeof(errbuf))) {
                fprintf(stderr, "[xi_pass] verify failed after round %d for '%s': %s\n", round,
                        f->name ? f->name : "?", errbuf);
                XR_DCHECK(false, "xi_pass: post-round verify failed");
            }
        }
#endif
    }

done:
    if (stats) {
        stats->total_rounds = (uint32_t) round;
        stats->total_ns = xr_time_monotonic_ns() - pipeline_start;
        stats->rpo_recomputes = f->rpo_recomputes;
        stats->dom_recomputes = f->dom_recomputes;
        stats->loop_recomputes = f->loop_recomputes;
    }

    /* Recurse into nested functions / closures */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i]) {
            XiPipelineStats child_stats;
            XiPipelineStats *child_stats_ptr = stats ? &child_stats : NULL;
            XiPassChange child_chg = xi_opt_run_pipeline_ex_with_mask(
                f->children[i], level, child_stats_ptr, budget_ns, disabled_passes);
            total = xi_pass_merge(total, child_chg);
            if (stats)
                stats_merge(stats, &child_stats);
        }
    }

    return total;
}

XR_FUNC XiPassChange xi_opt_run_pipeline_ex(XiFunc *f, XiOptLevel level, XiPipelineStats *stats,
                                            uint64_t budget_ns) {
    return xi_opt_run_pipeline_ex_with_mask(f, level, stats, budget_ns, XI_OPT_DISABLE_NONE);
}

XR_FUNC XiPassChange xi_opt_run_pipeline(XiFunc *f, XiOptLevel level) {
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
