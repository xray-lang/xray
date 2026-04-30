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
#include "../base/xmalloc.h"
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
XR_FUNC void xi_opt_strength_reduce(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_strength_reduce: NULL func");

    for (uint32_t b = 0; b < f->nblocks; b++) {
        XiBlock *blk = f->blocks[b];

        for (uint32_t i = 0; i < blk->nvalues; i++) {
            XiValue *v = blk->values[i];
            if (v->nargs != 2) continue;

            XiValue *lhs = v->args[0];
            XiValue *rhs = v->args[1];
            bool l_zero = is_const_int(lhs) && lhs->aux_int == 0;
            bool r_zero = is_const_int(rhs) && rhs->aux_int == 0;
            bool l_one  = is_const_int(lhs) && lhs->aux_int == 1;
            bool r_one  = is_const_int(rhs) && rhs->aux_int == 1;
            bool same   = (lhs == rhs);

            switch (v->op) {
                case XI_ADD:
                    /* x + 0 = x */
                    if (r_zero) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    /* 0 + x = x */
                    else if (l_zero) { v->op = XI_COPY; v->args[0] = rhs; v->nargs = 1; }
                    break;

                case XI_SUB:
                    /* x - 0 = x */
                    if (r_zero) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    /* x - x = 0 */
                    else if (same) { v->op = XI_CONST; v->aux_int = 0; v->nargs = 0; }
                    break;

                case XI_MUL:
                    /* x * 0 = 0 */
                    if (r_zero || l_zero) { v->op = XI_CONST; v->aux_int = 0; v->nargs = 0; }
                    /* x * 1 = x */
                    else if (r_one) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    /* 1 * x = x */
                    else if (l_one) { v->op = XI_COPY; v->args[0] = rhs; v->nargs = 1; }
                    break;

                case XI_DIV:
                    /* x / 1 = x */
                    if (r_one) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    break;

                case XI_BAND:
                    /* x & 0 = 0 */
                    if (r_zero || l_zero) { v->op = XI_CONST; v->aux_int = 0; v->nargs = 0; }
                    /* x & x = x */
                    else if (same) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    break;

                case XI_BOR:
                    /* x | 0 = x */
                    if (r_zero) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    else if (l_zero) { v->op = XI_COPY; v->args[0] = rhs; v->nargs = 1; }
                    /* x | x = x */
                    else if (same) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    break;

                case XI_BXOR:
                    /* x ^ 0 = x */
                    if (r_zero) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    else if (l_zero) { v->op = XI_COPY; v->args[0] = rhs; v->nargs = 1; }
                    /* x ^ x = 0 */
                    else if (same) { v->op = XI_CONST; v->aux_int = 0; v->nargs = 0; }
                    break;

                case XI_SHL:
                case XI_SHR:
                    /* x << 0 = x, x >> 0 = x */
                    if (r_zero) { v->op = XI_COPY; v->args[0] = lhs; v->nargs = 1; }
                    break;

                default:
                    break;
            }
        }
    }
}

/* ========== SelectRepresentations ========== */

/*
 * Determine the machine representation a value naturally produces.
 * Constants and arithmetic with known numeric types produce I64/F64.
 * Calls, loads, parameters, and polymorphic ops produce TAGGED.
 */
static XrRep sr_def_rep(const XiValue *v) {
    if (!v || !v->type) return XR_REP_TAGGED;
    switch (v->op) {
        case XI_CONST: {
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_NEG:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_BNOT:
        case XI_SHL: case XI_SHR: {
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
        case XI_NOT: case XI_ISNULL: case XI_IS:
            return XR_REP_I64;
        case XI_BOX:
            return XR_REP_TAGGED;
        case XI_UNBOX:
            return xr_type_base_rep(v->type);
        case XI_CONVERT: {
            XrRep r = xr_type_base_rep(v->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
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
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD:
        case XI_NEG:
        case XI_BAND: case XI_BOR: case XI_BXOR: case XI_BNOT:
        case XI_SHL: case XI_SHR: {
            XrRep r = xr_type_base_rep(user->type);
            return (r == XR_REP_I64 || r == XR_REP_F64) ? r : XR_REP_TAGGED;
        }
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE:
            if (arg_idx < user->nargs && user->args[arg_idx] &&
                user->args[arg_idx]->type) {
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
        default:
            return XR_REP_TAGGED;
    }
}

/* Allocate a BOX/UNBOX value in the arena without appending to the block. */
static XiValue *sr_make_convert(XiFunc *f, XiBlock *blk, uint16_t op,
                                 struct XrType *type, XiValue *arg) {
    XR_DCHECK(f != NULL, "sr_make_convert: NULL func");
    XR_DCHECK(blk != NULL, "sr_make_convert: NULL block");
    XR_DCHECK(arg != NULL, "sr_make_convert: NULL arg");
    XiValue *v = (XiValue *)xi_func_arena_alloc(f, sizeof(XiValue));
    if (!v) return NULL;
    memset(v, 0, sizeof(XiValue));
    v->id = f->next_value_id++;
    v->op = op;
    v->type = type;
    v->nargs = 1;
    v->uses = -1;
    v->block = blk;
    v->args = (XiValue **)xi_func_arena_alloc(f, sizeof(XiValue *));
    if (!v->args) return NULL;
    v->args[0] = arg;
    return v;
}

/* Rewrite a single arg reference if rep mismatches. */
static void sr_rewrite_arg(XiFunc *f, XiValue **arg_slot,
                            XrRep use_r, XiValue **box_of, XiValue **unbox_of,
                            uint32_t max_id) {
    XiValue *arg = *arg_slot;
    if (!arg || arg->id >= max_id) return;
    XrRep def_r = sr_def_rep(arg);
    if (def_r == use_r) return;

    if (def_r != XR_REP_TAGGED && use_r == XR_REP_TAGGED) {
        /* Unboxed -> tagged: insert BOX */
        if (!box_of[arg->id]) {
            box_of[arg->id] = sr_make_convert(f, arg->block, XI_BOX,
                                               arg->type, arg);
        }
        if (box_of[arg->id]) *arg_slot = box_of[arg->id];
    } else if (def_r == XR_REP_TAGGED && use_r != XR_REP_TAGGED) {
        /* Tagged -> unboxed: insert UNBOX */
        if (!unbox_of[arg->id]) {
            unbox_of[arg->id] = sr_make_convert(f, arg->block, XI_UNBOX,
                                                  arg->type, arg);
        }
        if (unbox_of[arg->id]) *arg_slot = unbox_of[arg->id];
    }
}

XR_FUNC void xi_opt_select_rep(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_select_rep: NULL func");

    uint32_t max_id = f->next_value_id;
    if (max_id == 0) return;

    XiValue **box_of = (XiValue **)xr_calloc(max_id, sizeof(XiValue *));
    XiValue **unbox_of = (XiValue **)xr_calloc(max_id, sizeof(XiValue *));
    if (!box_of || !unbox_of) { xr_free(box_of); xr_free(unbox_of); return; }

    /* Rewrite args of every instruction, phi, and block control. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk) continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v) continue;
            for (uint16_t ai = 0; ai < v->nargs; ai++) {
                XrRep use_r = sr_use_rep(v, ai);
                sr_rewrite_arg(f, &v->args[ai], use_r,
                               box_of, unbox_of, max_id);
            }
        }

        /* Phi args: force TAGGED (merge point, safe default) */
        for (XiPhi *phi = blk->phis; phi; phi = phi->next) {
            for (uint16_t ai = 0; ai < phi->value.nargs; ai++) {
                sr_rewrite_arg(f, &phi->value.args[ai], XR_REP_TAGGED,
                               box_of, unbox_of, max_id);
            }
        }

        /* Return control: must be TAGGED */
        if (blk->kind == XI_BLOCK_RETURN && blk->control) {
            sr_rewrite_arg(f, &blk->control, XR_REP_TAGGED,
                           box_of, unbox_of, max_id);
        }
    }

    /* Rebuild each block's value array to include BOX/UNBOX after source. */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk) continue;

        uint32_t extra = 0;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v) continue;
            if (v->id < max_id && box_of[v->id] && box_of[v->id]->block == blk)
                extra++;
            if (v->id < max_id && unbox_of[v->id] && unbox_of[v->id]->block == blk)
                extra++;
        }
        if (extra == 0) continue;

        uint32_t new_cap = blk->nvalues + extra;
        XiValue **nv = (XiValue **)xi_func_arena_alloc(f, new_cap * sizeof(XiValue *));
        if (!nv) continue;

        uint32_t ni = 0;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            nv[ni++] = v;
            if (!v || v->id >= max_id) continue;
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

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_opt_select_rep(f->children[i]);
    }
}

/* ========== BOX/UNBOX Peephole Elimination ========== */

/*
 * Eliminate inverse BOX/UNBOX pairs:
 *   UNBOX(BOX(x)) -> COPY(x)
 *   BOX(UNBOX(x)) -> COPY(x)
 * Subsequent copy-prop and DCE clean up the COPY and dead BOX/UNBOX.
 */
XR_FUNC void xi_opt_box_elim(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_box_elim: NULL func");

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        XiBlock *blk = f->blocks[bi];
        if (!blk) continue;

        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            XiValue *v = blk->values[vi];
            if (!v || v->nargs < 1 || !v->args[0]) continue;

            XiValue *inner = v->args[0];
            if (inner->nargs < 1 || !inner->args[0]) continue;

            bool elim = (v->op == XI_UNBOX && inner->op == XI_BOX) ||
                        (v->op == XI_BOX && inner->op == XI_UNBOX);
            if (elim) {
                v->op = XI_COPY;
                v->args[0] = inner->args[0];
            }
        }
    }

    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_opt_box_elim(f->children[i]);
    }
}

/* ========== Combined Pass ========== */

XR_FUNC void xi_opt_run(XiFunc *f) {
    XR_DCHECK(f != NULL, "xi_opt_run: NULL func");

    xi_opt_const_fold(f);
    xi_opt_strength_reduce(f);
    xi_opt_copy_prop(f);
    xi_opt_phi_simplify(f);
    xi_opt_dce(f);

    /* Recurse into nested functions / closures */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_opt_run(f->children[i]);
    }
}
