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
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype.h"
#include <string.h>

/* ========== Helpers ========== */

/* Check if a value is a constant (integer or float). */
static bool is_const_int(const XiValue *v) {
    return v && v->op == XI_CONST && v->type &&
           v->type->kind == XR_KIND_INT;
}

static bool is_const_float(const XiValue *v) {
    return v && v->op == XI_CONST && v->type &&
           v->type->kind == XR_KIND_FLOAT;
}

static bool is_const_bool(const XiValue *v) {
    return v && v->op == XI_CONST && v->type &&
           v->type->kind == XR_KIND_BOOL;
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

/* Try to fold a binary op on two integer constants. */
static bool fold_int_binary(uint16_t op, int64_t a, int64_t b,
                             int64_t *result) {
    switch (op) {
        case XI_ADD:  *result = a + b; return true;
        case XI_SUB:  *result = a - b; return true;
        case XI_MUL:  *result = a * b; return true;
        case XI_DIV:
            if (b == 0) return false;
            *result = a / b;
            return true;
        case XI_MOD:
            if (b == 0) return false;
            *result = a % b;
            return true;
        case XI_BAND:  *result = a & b; return true;
        case XI_BOR:   *result = a | b; return true;
        case XI_BXOR:  *result = a ^ b; return true;
        case XI_SHL:   *result = a << (b & 63); return true;
        case XI_SHR:   *result = a >> (b & 63); return true;
        default: return false;
    }
}

/* Try to fold a comparison on two integer constants. */
static bool fold_int_compare(uint16_t op, int64_t a, int64_t b,
                              bool *result) {
    switch (op) {
        case XI_EQ: *result = (a == b); return true;
        case XI_NE: *result = (a != b); return true;
        case XI_LT: *result = (a < b);  return true;
        case XI_LE: *result = (a <= b); return true;
        case XI_GT: *result = (a > b);  return true;
        case XI_GE: *result = (a >= b); return true;
        default: return false;
    }
}

/* Try to fold a binary op on two float constants. */
static bool fold_float_binary(uint16_t op, double a, double b,
                               double *result) {
    switch (op) {
        case XI_ADD: *result = a + b; return true;
        case XI_SUB: *result = a - b; return true;
        case XI_MUL: *result = a * b; return true;
        case XI_DIV:
            if (b == 0.0) return false;
            *result = a / b;
            return true;
        default: return false;
    }
}

/* Try to fold a comparison on two float constants. */
static bool fold_float_compare(uint16_t op, double a, double b,
                                bool *result) {
    switch (op) {
        case XI_EQ: *result = (a == b); return true;
        case XI_NE: *result = (a != b); return true;
        case XI_LT: *result = (a < b);  return true;
        case XI_LE: *result = (a <= b); return true;
        case XI_GT: *result = (a > b);  return true;
        case XI_GE: *result = (a >= b); return true;
        default: return false;
    }
}

XR_FUNC void xi_opt_const_fold(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_const_fold: NULL func");

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];

            /* Fold unary NEG on const int */
            if (v->op == XI_NEG && v->nargs == 1 &&
                is_const_int(v->args[0])) {
                v->op = XI_CONST;
                v->aux_int = -(v->args[0]->aux_int);
                v->nargs = 0;
                continue;
            }

            /* Fold unary NEG on const float */
            if (v->op == XI_NEG && v->nargs == 1 &&
                is_const_float(v->args[0])) {
                double val;
                memcpy(&val, &v->args[0]->aux_int, sizeof(double));
                val = -val;
                v->op = XI_CONST;
                memcpy(&v->aux_int, &val, sizeof(double));
                v->nargs = 0;
                continue;
            }

            /* Fold unary NOT on const bool */
            if (v->op == XI_NOT && v->nargs == 1 &&
                is_const_bool(v->args[0])) {
                v->op = XI_CONST;
                v->aux_int = v->args[0]->aux_int ? 0 : 1;
                v->nargs = 0;
                continue;
            }

            /* Fold unary BNOT on const int */
            if (v->op == XI_BNOT && v->nargs == 1 &&
                is_const_int(v->args[0])) {
                v->op = XI_CONST;
                v->aux_int = ~(v->args[0]->aux_int);
                v->nargs = 0;
                continue;
            }

            /* Binary: need exactly 2 args */
            if (v->nargs != 2) continue;
            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];

            /* Integer binary/compare */
            if (is_const_int(lhs) && is_const_int(rhs)) {
                int64_t result;
                if (fold_int_binary(v->op, lhs->aux_int, rhs->aux_int,
                                     &result)) {
                    v->op = XI_CONST;
                    v->aux_int = result;
                    v->nargs = 0;
                    continue;
                }
                bool bres;
                if (fold_int_compare(v->op, lhs->aux_int, rhs->aux_int,
                                      &bres)) {
                    v->op = XI_CONST;
                    v->aux_int = bres ? 1 : 0;
                    v->nargs = 0;
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
                    continue;
                }
                bool bres;
                if (fold_float_compare(v->op, a, b, &bres)) {
                    v->op = XI_CONST;
                    v->aux_int = bres ? 1 : 0;
                    v->nargs = 0;
                    continue;
                }
            }
        }
    }
}

/* ========== Copy Propagation ========== */

/* Resolve through XI_COPY chains to find the original source. */
static XiValue *resolve_copy(XiValue *v) {
    while (v && v->op == XI_COPY && v->nargs >= 1)
        v = v->args[0];
    return v;
}

XR_FUNC void xi_opt_copy_prop(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_copy_prop: NULL func");

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        /* Rewrite args of each value */
        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            for (uint16_t a = 0; a < v->nargs; a++) {
                XiValue *resolved = resolve_copy(v->args[a]);
                if (resolved && resolved != v->args[a])
                    v->args[a] = resolved;
            }
        }

        /* Rewrite phi operands */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t a = 0; a < phi->value.nargs; a++) {
                XiValue *resolved = resolve_copy(phi->value.args[a]);
                if (resolved && resolved != phi->value.args[a])
                    phi->value.args[a] = resolved;
            }
        }

        /* Rewrite block control */
        if (blk->control) {
            XiValue *resolved = resolve_copy(blk->control);
            if (resolved && resolved != blk->control)
                blk->control = resolved;
        }
    }
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

XR_FUNC void xi_opt_dce(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_dce: NULL func");

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

                /* Keep if: has uses, has side effects, or is a terminator-related */
                if (v->uses > 0 || (v->flags & XI_FLAG_SIDE_EFFECT)) {
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
                /* Don't increment i: next value shifted into position */
            }
        }
    }
}

/* ========== Phi Simplification ========== */

XR_FUNC void xi_opt_phi_simplify(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_phi_simplify: NULL func");

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
                        continue;  /* skip self-references and NULLs */
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
                } else {
                    prev_ptr = &phi->next;
                }

                phi = next;
            }
        }
    }
}

/* ========== Combined Pass ========== */

XR_FUNC void xi_opt_run(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_run: NULL func");

    xi_opt_const_fold(f);
    xi_opt_copy_prop(f);
    xi_opt_phi_simplify(f);
    xi_opt_dce(f);
}
