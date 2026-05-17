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
#include "xi_opt_gvn.h"
#include "xi_opt_ifconv.h"
#include "xi_opt_inline.h"
#include "xi_opt_licm.h"
#include "xi_opt_sccp.h"
#include "xi_pass.h"
#include "xi_verify.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../os/os_time.h"
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
 * runtime VM and JIT, which also produce INT64_MIN / 0 respectively.
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

/* Try to fold a binary op on two float constants. */
static bool fold_float_binary(uint16_t op, double a, double b, double *result) {
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

XR_FUNC XiPassChange xi_opt_const_fold(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_const_fold: NULL func");
    XiPassChange chg = xi_pass_no_change();

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];

            /* Fold unary NEG on const int.
             * -INT64_MIN is UB on signed; negate on uint64_t then cast back
             * to preserve wrap-on-overflow semantics (matches VM and JIT). */
            if (v->op == XI_NEG && v->nargs == 1 && is_const_int(v->args[0])) {
                v->op = XI_CONST;
                v->aux_int = (int64_t) (0u - (uint64_t) v->args[0]->aux_int);
                v->nargs = 0;
                chg.values_changed = true;
                continue;
            }

            /* Fold unary NEG on const float */
            if (v->op == XI_NEG && v->nargs == 1 && is_const_float(v->args[0])) {
                double val;
                memcpy(&val, &v->args[0]->aux_int, sizeof(double));
                val = -val;
                v->op = XI_CONST;
                memcpy(&v->aux_int, &val, sizeof(double));
                v->nargs = 0;
                chg.values_changed = true;
                continue;
            }

            /* Fold unary NOT on const bool */
            if (v->op == XI_NOT && v->nargs == 1 && is_const_bool(v->args[0])) {
                v->op = XI_CONST;
                v->aux_int = v->args[0]->aux_int ? 0 : 1;
                v->nargs = 0;
                chg.values_changed = true;
                continue;
            }

            /* Fold unary BNOT on const int */
            if (v->op == XI_BNOT && v->nargs == 1 && is_const_int(v->args[0])) {
                v->op = XI_CONST;
                v->aux_int = ~(v->args[0]->aux_int);
                v->nargs = 0;
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
                    v->op = XI_COPY;
                    v->args[0] = tup->args[(uint16_t) idx];
                    v->aux_int = 0;
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
                    v->op = XI_CONST;
                    v->aux_int = result;
                    v->nargs = 0;
                    chg.values_changed = true;
                    continue;
                }
                bool bres;
                if (fold_int_compare(v->op, lhs->aux_int, rhs->aux_int, &bres)) {
                    v->op = XI_CONST;
                    v->aux_int = bres ? 1 : 0;
                    v->nargs = 0;
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
                if (fold_float_binary(v->op, a, b, &dresult)) {
                    v->op = XI_CONST;
                    memcpy(&v->aux_int, &dresult, sizeof(double));
                    v->nargs = 0;
                    chg.values_changed = true;
                    continue;
                }
                bool bres;
                if (fold_float_compare(v->op, a, b, &bres)) {
                    v->op = XI_CONST;
                    v->aux_int = bres ? 1 : 0;
                    v->nargs = 0;
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
        if (v->var_id != 0xFF && src && src->var_id != 0xFF && v->var_id != src->var_id)
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
                    (arg->var_id == 0xFF || arg->var_id == resolved->var_id)) {
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

/* ========== Strength Reduction ========== */

/*
 * Algebraic identity rewrites for integer binary operations.
 * Converts a binary op to either a COPY (identity) or CONST (zero/absorb).
 *
 * Patterns handled:
 *   x + 0 = x,  0 + x = x
 *   x - 0 = x
 *   x * 1 = x,  1 * x = x
 *   x * 0 = 0,  0 * x = 0
 *   x / 1 = x
 *   x & 0 = 0,  0 & x = 0
 *   x | 0 = x,  0 | x = x
 *   x ^ 0 = x,  0 ^ x = x
 *   x << 0 = x, x >> 0 = x
 *   x - x = 0
 *   x ^ x = 0
 */
XR_FUNC XiPassChange xi_opt_strength_reduce(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_strength_reduce: NULL func");
    XiPassChange chg = xi_pass_no_change();

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v->nargs != 2)
                continue;

            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];
            bool l_zero = is_const_int(lhs) && lhs->aux_int == 0;
            bool r_zero = is_const_int(rhs) && rhs->aux_int == 0;
            bool l_one = is_const_int(lhs) && lhs->aux_int == 1;
            bool r_one = is_const_int(rhs) && rhs->aux_int == 1;
            bool same = (lhs == rhs);

            switch (v->op) {
                case XI_ADD:
                    /* x + 0 = x */
                    if (r_zero) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    /* 0 + x = x */
                    else if (l_zero) {
                        v->op = XI_COPY;
                        v->args[0] = rhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    break;

                case XI_SUB:
                    /* x - 0 = x */
                    if (r_zero) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    /* x - x = 0 */
                    else if (same) {
                        v->op = XI_CONST;
                        v->aux_int = 0;
                        v->nargs = 0;
                        chg.values_changed = true;
                    }
                    break;

                case XI_MUL: {
                    /* x * 0 = 0 (only valid for numeric types; string * 0
                     * must go through the runtime to produce "") */
                    bool both_numeric = lhs->type && rhs->type &&
                                        lhs->type->kind != XR_KIND_STRING &&
                                        rhs->type->kind != XR_KIND_STRING;
                    if ((r_zero || l_zero) && both_numeric) {
                        v->op = XI_CONST;
                        v->aux_int = 0;
                        v->nargs = 0;
                        chg.values_changed = true;
                    }
                    /* x * 1 = x */
                    else if (r_one) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    /* 1 * x = x */
                    else if (l_one) {
                        v->op = XI_COPY;
                        v->args[0] = rhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    break;
                }

                case XI_DIV:
                    /* x / 1 = x */
                    if (r_one) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    break;

                case XI_BAND:
                    /* x & 0 = 0 */
                    if (r_zero || l_zero) {
                        v->op = XI_CONST;
                        v->aux_int = 0;
                        v->nargs = 0;
                        chg.values_changed = true;
                    }
                    /* x & x = x */
                    else if (same) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    break;

                case XI_BOR:
                    /* x | 0 = x */
                    if (r_zero) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    } else if (l_zero) {
                        v->op = XI_COPY;
                        v->args[0] = rhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    /* x | x = x */
                    else if (same) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    break;

                case XI_BXOR:
                    /* x ^ 0 = x */
                    if (r_zero) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    } else if (l_zero) {
                        v->op = XI_COPY;
                        v->args[0] = rhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    /* x ^ x = 0 */
                    else if (same) {
                        v->op = XI_CONST;
                        v->aux_int = 0;
                        v->nargs = 0;
                        chg.values_changed = true;
                    }
                    break;

                case XI_SHL:
                case XI_SHR:
                    /* x << 0 = x, x >> 0 = x */
                    if (r_zero) {
                        v->op = XI_COPY;
                        v->args[0] = lhs;
                        v->nargs = 1;
                        chg.values_changed = true;
                    }
                    break;

                default:
                    break;
            }
        }
    }
    return chg;
}

/* ========== SelectRepresentations ========== */

/* Determine the machine representation a value naturally produces.
 * Constants and arithmetic with known numeric types produce I64/F64.
 * Calls, loads, and polymorphic ops produce TAGGED. */
static XrRep sr_def_rep(const XiValue *v) {
    if (!v || !v->type)
        return XR_REP_TAGGED;
    switch (v->op) {
        case XI_PARAM: {
            /* Typed scalar params get concrete rep.  The JIT/AOT can
             * use this directly instead of re-inferring from type->kind. */
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        case XI_CONST: {
            if (v->type->kind == XR_KIND_NULL || v->type->kind == XR_KIND_STRING)
                return XR_REP_TAGGED;
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
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
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
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
            return XR_REP_I64;
        case XI_BOX:
            return XR_REP_TAGGED;
        case XI_UNBOX: {
            XrRep ur = xr_type_base_rep(v->type);
            if (ur == XR_REP_I64 || ur == XR_REP_F64)
                return ur;
            if (v->nargs >= 1 && v->args[0] && v->args[0]->type) {
                ur = xr_type_base_rep(v->args[0]->type);
                if (ur == XR_REP_I64 || ur == XR_REP_F64)
                    return ur;
            }
            return XR_REP_TAGGED;
        }
        case XI_CONVERT: {
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
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
static XrRep sr_use_rep(const XiValue *user, uint16_t arg_idx) {
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
            XrRep r = xr_type_base_rep(user->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        case XI_EQ:
        case XI_NE:
        case XI_LT:
        case XI_LE:
        case XI_GT:
        case XI_GE:
        case XI_EQ_STRICT:
        case XI_NE_STRICT:
            if (arg_idx < user->nargs && user->args[arg_idx] && user->args[arg_idx]->type) {
                XrRep r = xr_type_base_rep(user->args[arg_idx]->type);
                return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
            }
            return XR_REP_TAGGED;
        case XI_NOT:
            return XR_REP_I64;
        case XI_BOX:
            if (user->args[0] && user->args[0]->type) {
                XrRep r = xr_type_base_rep(user->args[0]->type);
                return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
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
                           XiValue **unbox_of, uint32_t max_id) {
    XiValue *arg = *arg_slot;
    if (!arg || arg->id >= max_id)
        return;
    XrRep def_r = sr_def_rep(arg);
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

XR_FUNC XiPassChange xi_opt_select_rep(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_select_rep: NULL func");

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
                XrRep use_r = sr_use_rep(v, ai);
                sr_rewrite_arg(f, &v->args[ai], use_r, box_of, unbox_of, max_id);
            }
        }

        /* Phi args: force TAGGED (merge point, safe default) */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                sr_rewrite_arg(f, &phi->value.args[ai], XR_REP_TAGGED, box_of, unbox_of, max_id);
            }
        }

        /* Return control: must be TAGGED */
        if (blk->kind == XI_BLOCK_RETURN && blk->control) {
            sr_rewrite_arg(f, &blk->control, XR_REP_TAGGED, box_of, unbox_of, max_id);
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
            xi_opt_select_rep(f->children[i]);
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
                v->rep = (uint8_t) sr_def_rep(v);
        }
        for (XiPhi *phi = blk->phis; phi; phi = phi->next)
            phi->value.rep = XR_REP_TAGGED;
    }

    f->stage = XI_STAGE_REPPED;
    f->invariant_mask |= xi_stage_invariants(XI_STAGE_REPPED);
    return xi_pass_change_all();
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
                v->op = XI_COPY;
                v->args[0] = inner->args[0];
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
       out_stage */
    {"constfold", xi_opt_const_fold, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW},
    {"strength_reduce", xi_opt_strength_reduce, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW,
     XI_STAGE_RAW},
    {"copy_prop", xi_opt_copy_prop, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW},
    {"phi_simplify", xi_opt_phi_simplify, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW},
    {"dce", xi_opt_dce, XI_OPT_LIGHT, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW},
    {"sccp", xi_opt_sccp, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW},
    {"gvn", xi_opt_gvn, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW},
    {"licm", xi_opt_licm, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW},
    {"inline", xi_opt_inline, XI_OPT_FULL, XI_PASS_NONE, XI_STAGE_RAW, XI_STAGE_RAW},
    {"ifconv", xi_opt_ifconv, XI_OPT_FULL, XI_PASS_NEEDS_DOM, XI_STAGE_RAW, XI_STAGE_RAW},
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
    xi_pass_order_check();
}

/* ========== Pass Order Constraints ========== */

/* Declarative ordering rules. Each entry says "before must appear
 * earlier than after in the pass table".  Checked once at startup. */
static const XiPassOrderConstraint xi_pass_constraints[] = {
    /* Within the same opt level, ordering matters for efficiency.
     * Cross-level constraints (e.g. SCCP -> DCE) are not enforced
     * here because the fixed-point loop handles convergence. */
    {"constfold", "copy_prop", "constant folding enables more copy propagation"},
    {"copy_prop", "dce", "copy propagation makes original values dead"},
    {"gvn", "licm", "GVN eliminates redundancies before LICM hoists"},
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

/* Randomized topological sort of values within a block.
 * Respects: (1) intra-block data dependencies (use after def),
 *           (2) relative order among memory-touching / effectful values.
 * Uses Kahn's algorithm with random selection from the ready set. */
static void shuffle_block_values(XiBlock *blk) {
    uint32_t n = blk->nvalues;
    if (n <= 1)
        return;

    /* Small stack buffer; fall back to heap for large blocks. */
    uint32_t stack_indeg[128];
    uint32_t stack_ready[128];
    uint32_t *indeg = (n <= 128) ? stack_indeg : (uint32_t *) xr_malloc(n * sizeof(uint32_t));
    uint32_t *ready = (n <= 128) ? stack_ready : (uint32_t *) xr_malloc(n * sizeof(uint32_t));
    if (!indeg || !ready)
        return;

    memset(indeg, 0, n * sizeof(uint32_t));

    /* Build dependency graph.
     * Edge types:
     *   (a) Data: value[j] uses value[i] (same block) → edge i→j
     *   (b) Effect chain: consecutive effectful values maintain order
     *
     * For (a), we only need arg->block == blk to confirm intra-block.
     * Phi operands are never in the values[] array, so they won't match. */

    int32_t prev_effect = -1;
    for (uint32_t i = 0; i < n; i++) {
        XiValue *v = blk->values[i];

        /* (a) Data dependencies: count intra-block operands.
         * Exclude phis (op == XI_PHI) — they live in blk->phis,
         * not blk->values[], so they are never "placed" by the
         * topo sort and would create false cycles. */
        for (uint16_t a = 0; a < v->nargs; a++) {
            XiValue *arg = v->args[a];
            if (arg && arg->block == blk && arg->op != XI_PHI) {
                indeg[i]++;
            }
        }

        /* (b) Memory/effect chain: values touching memory or having
         * side effects maintain relative order.  READS_MEM is included
         * because a read must not move before a preceding write. */
        if (v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM |
                        XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND)) {
            if (prev_effect >= 0)
                indeg[i]++;
            prev_effect = (int32_t) i;
        }
    }

    /* Collect initially ready values */
    uint32_t ready_count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (indeg[i] == 0)
            ready[ready_count++] = i;
    }

    /* Kahn's algorithm with random pick */
    XiValue **result = blk->values;
    XiValue *stack_out[128];
    XiValue **out = (n <= 128) ? stack_out : (XiValue **) xr_malloc(n * sizeof(XiValue *));
    if (!out)
        goto cleanup;

    uint32_t placed = 0;

    /* Precompute: for each effectful value, its immediate predecessor
     * in the effect chain (original order). */
    int32_t stack_epred[128];
    int32_t *effect_pred = (n <= 128) ? stack_epred : (int32_t *) xr_malloc(n * sizeof(int32_t));
    if (!effect_pred)
        goto cleanup;
    {
        int32_t pe = -1;
        for (uint32_t i = 0; i < n; i++) {
            effect_pred[i] = -1;
            if (result[i]->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM |
                                    XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND)) {
                effect_pred[i] = pe;
                pe = (int32_t) i;
            }
        }
    }

    while (ready_count > 0) {
        uint32_t pick = (uint32_t) (rand() % ready_count);
        uint32_t idx = ready[pick];
        ready[pick] = ready[--ready_count];

        out[placed++] = result[idx];
        XiValue *placed_v = result[idx];

        /* Mark as placed using sentinel in-degree */
        indeg[idx] = UINT32_MAX;

        bool placed_is_effect =
            (placed_v->flags & (XI_FLAG_SIDE_EFFECT | XI_FLAG_READS_MEM | XI_FLAG_WRITES_MEM |
                                XI_FLAG_MAY_THROW | XI_FLAG_MAY_SUSPEND)) != 0;

        /* Release dependents */
        for (uint32_t j = 0; j < n; j++) {
            if (indeg[j] == 0 || indeg[j] == UINT32_MAX)
                continue;

            XiValue *vj = result[j];
            uint32_t released = 0;

            /* (a) Data dependency: vj uses placed_v (count all occurrences,
             * since indeg was incremented per-arg during construction).
             * Only count non-phi args (matches the build phase filter). */
            for (uint16_t a = 0; a < vj->nargs; a++) {
                if (vj->args[a] == placed_v && placed_v->op != XI_PHI)
                    released++;
            }

            /* (b) Effect chain: vj's immediate effect predecessor is idx */
            if (placed_is_effect && effect_pred[j] == (int32_t) idx) {
                released++;
            }

            if (released > 0) {
                XR_DCHECK(indeg[j] >= released, "shuffle_block_values: indeg underflow");
                indeg[j] -= released;
                if (indeg[j] == 0)
                    ready[ready_count++] = j;
            }
        }
    }

    /* If topo sort is incomplete, a real cycle exists (should not happen
     * in well-formed Xi IR). Skip the shuffle silently in release. */
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
        if (indeg != stack_indeg)
            xr_free(indeg);
        if (ready != stack_ready)
            xr_free(ready);
        if (out != stack_out)
            xr_free(out);
        if (effect_pred != stack_epred)
            xr_free(effect_pred);
    }
}

XR_FUNC XiPassChange xi_opt_run_pipeline_ex(XiFunc *f, XiOptLevel level, XiPipelineStats *stats,
                                            uint64_t budget_ns) {
    XR_DCHECK(f != NULL, "xi_opt_run_pipeline_ex: NULL func");

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
        if (shuffle_blocks)
            srand((unsigned) xr_time_monotonic_ns());
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

            /* Shuffle blocks[1..n-1] (preserve entry at [0]) to catch
             * passes that assume RPO or insertion order. */
            if (shuffle_blocks && f->nblocks > 2) {
                for (uint32_t si = f->nblocks - 1; si > 1; si--) {
                    uint32_t sj = 1 + (uint32_t) (rand() % si);
                    XiBlock *tmp = f->blocks[si];
                    f->blocks[si] = f->blocks[sj];
                    f->blocks[sj] = tmp;
                }
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
    }

    /* Recurse into nested functions / closures */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i]) {
            XiPassChange child_chg = xi_opt_run_pipeline_ex(f->children[i], level, NULL, budget_ns);
            total = xi_pass_merge(total, child_chg);
        }
    }

    return total;
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
    for (uint32_t i = 0; i < stats->npass; i++) {
        const XiPassStats *ps = &stats->passes[i];
        if (ps->invocations == 0)
            continue;
        fprintf(stderr, "  %-18s  %3u calls  %5u rem  %5u add  %7.3f ms\n", ps->name,
                ps->invocations, ps->n_removed, ps->n_added, (double) ps->elapsed_ns / 1e6);
    }
}
