/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_fold.c - On-the-fly peephole optimization (FOLD engine)
 *
 * KEY CONCEPT:
 *   Applied at IR construction time inside xi_to_xm. Each call to
 *   xm_fold_emit() checks a table of algebraic rules before delegating
 *   to xm_emit(). When a rule matches, the instruction is eliminated
 *   and an existing XmRef is returned instead.
 *
 * WHY THIS DESIGN:
 *   - Zero-cost when no rule matches (single switch + few comparisons)
 *   - Reduces IR size early, benefiting all downstream passes
 *   - Complements (not replaces) existing post-hoc passes
 */

#include "xm_fold.h"
#include "../base/xchecks.h"
#include <string.h>

/* ========== Helper: query constant value of a ref ========== */

/*
 * If ref is a vreg whose defining instruction is CONST_I64 loading a
 * constant pool entry, return true and write the i64 value to *out.
 */
static bool fold_get_const_i64(XmFunc *func, XmRef ref, int64_t *out) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return false;
    XmIns *def = func->vregs[idx].def;
    if (!def || def->op != XM_CONST_I64)
        return false;
    if (!xm_ref_is_const(def->args[0]))
        return false;
    uint32_t cidx = XM_REF_INDEX(def->args[0]);
    if (cidx >= func->nconst)
        return false;
    *out = func->consts[cidx].val.i64;
    return true;
}

static bool fold_get_const_f64(XmFunc *func, XmRef ref, double *out) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return false;
    XmIns *def = func->vregs[idx].def;
    if (!def || def->op != XM_CONST_F64)
        return false;
    if (!xm_ref_is_const(def->args[0]))
        return false;
    uint32_t cidx = XM_REF_INDEX(def->args[0]);
    if (cidx >= func->nconst)
        return false;
    *out = func->consts[cidx].val.f64;
    return true;
}

/* ========== Helper: query the defining opcode of a vreg ========== */

static uint16_t fold_def_op(XmFunc *func, XmRef ref) {
    if (!xm_ref_is_vreg(ref))
        return XM_NOP;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return XM_NOP;
    XmIns *def = func->vregs[idx].def;
    return def ? def->op : XM_NOP;
}

// Get the first argument of a vreg's defining instruction
static XmRef fold_def_arg0(XmFunc *func, XmRef ref) {
    if (!xm_ref_is_vreg(ref))
        return XM_NONE;
    uint32_t idx = XM_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return XM_NONE;
    XmIns *def = func->vregs[idx].def;
    return def ? def->args[0] : XM_NONE;
}

/* ========== Integer binary fold rules ========== */

/*
 * Try to fold an integer binary operation.
 * Returns true if folded, with *result set to the replacement ref.
 */
static bool fold_int_binary(XmFunc *func, XmBlock *blk, uint16_t op, XmRef a, XmRef b,
                            XmRef *result) {
    XR_DCHECK(func != NULL, "fold_int_binary: NULL func");
    XR_DCHECK(result != NULL, "fold_int_binary: NULL result");
    int64_t va, vb;
    bool a_const = fold_get_const_i64(func, a, &va);
    bool b_const = fold_get_const_i64(func, b, &vb);

    /* Rule 1: identity / annihilation with right constant
     * Checked before constant folding because returning an existing
     * vreg is strictly cheaper than creating a new CONST_I64. */
    if (b_const) {
        switch (op) {
            case XM_ADD:
            case XM_SUB:
            case XM_OR:
            case XM_XOR:
            case XM_SHL:
            case XM_SHR:
                if (vb == 0) {
                    *result = a;
                    return true;
                }
                break;
            case XM_MUL:
                if (vb == 1) {
                    *result = a;
                    return true;
                }
                if (vb == 0) {
                    XmRef cref = xm_const_i64(func, 0);
                    *result = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
                    return true;
                }
                break;
            case XM_DIV:
                if (vb == 1) {
                    *result = a;
                    return true;
                }
                break;
            case XM_AND:
                if (vb == -1) {
                    *result = a;
                    return true;
                }  // x & 0xFFFFFFFFFFFFFFFF
                if (vb == 0) {
                    XmRef cref = xm_const_i64(func, 0);
                    *result = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
                    return true;
                }
                break;
            default:
                break;
        }
    }

    // Rule 2: identity with left constant
    if (a_const) {
        switch (op) {
            case XM_ADD:
            case XM_OR:
            case XM_XOR:
                if (va == 0) {
                    *result = b;
                    return true;
                }
                break;
            case XM_MUL:
                if (va == 1) {
                    *result = b;
                    return true;
                }
                if (va == 0) {
                    XmRef cref = xm_const_i64(func, 0);
                    *result = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
                    return true;
                }
                break;
            case XM_AND:
                if (va == -1) {
                    *result = b;
                    return true;
                }
                if (va == 0) {
                    XmRef cref = xm_const_i64(func, 0);
                    *result = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
                    return true;
                }
                break;
            default:
                break;
        }
    }

    // Rule 3: self-operation (same vreg for both operands)
    if (a == b) {
        switch (op) {
            case XM_SUB:
            case XM_XOR: {
                XmRef cref = xm_const_i64(func, 0);
                *result = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
                return true;
            }
            case XM_AND:
            case XM_OR:
                *result = a;
                return true;
            default:
                break;
        }
    }

    // Rule 4: constant folding (both operands known)
    if (a_const && b_const) {
        int64_t r = 0;
        bool ok = true;
        switch (op) {
            case XM_ADD:
                r = va + vb;
                break;
            case XM_SUB:
                r = va - vb;
                break;
            case XM_MUL:
                r = va * vb;
                break;
            case XM_DIV:
                if (vb == 0) {
                    ok = false;
                    break;
                }
                r = va / vb;
                break;
            case XM_MOD:
                if (vb == 0) {
                    ok = false;
                    break;
                }
                r = va % vb;
                break;
            case XM_AND:
                r = va & vb;
                break;
            case XM_OR:
                r = va | vb;
                break;
            case XM_XOR:
                r = va ^ vb;
                break;
            case XM_SHL:
                r = va << (vb & 63);
                break;
            case XM_SHR:
                r = (int64_t) ((uint64_t) va >> (vb & 63));
                break;
            case XM_EQ:
                r = (va == vb) ? 1 : 0;
                break;
            case XM_NE:
                r = (va != vb) ? 1 : 0;
                break;
            case XM_LT:
                r = (va < vb) ? 1 : 0;
                break;
            case XM_LE:
                r = (va <= vb) ? 1 : 0;
                break;
            case XM_GT:
                r = (va > vb) ? 1 : 0;
                break;
            case XM_GE:
                r = (va >= vb) ? 1 : 0;
                break;
            default:
                ok = false;
                break;
        }
        if (ok) {
            XmRef cref = xm_const_i64(func, r);
            *result = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
            return true;
        }
    }

    return false;
}

/* ========== Float binary fold rules ========== */

static bool fold_float_binary(XmFunc *func, XmBlock *blk, uint16_t op, XmRef a, XmRef b,
                              uint8_t type, XmRef *result) {
    (void) type;
    double va, vb;
    bool a_const = fold_get_const_f64(func, a, &va);
    bool b_const = fold_get_const_f64(func, b, &vb);

    // Rule 1: identity with right constant (before constant folding)
    if (b_const) {
        switch (op) {
            case XM_FADD:
            case XM_FSUB:
                if (vb == 0.0) {
                    *result = a;
                    return true;
                }
                break;
            case XM_FMUL:
                if (vb == 1.0) {
                    *result = a;
                    return true;
                }
                break;
            case XM_FDIV:
                if (vb == 1.0) {
                    *result = a;
                    return true;
                }
                break;
            default:
                break;
        }
    }

    // Rule 2: identity with left constant
    if (a_const) {
        switch (op) {
            case XM_FADD:
                if (va == 0.0) {
                    *result = b;
                    return true;
                }
                break;
            case XM_FMUL:
                if (va == 1.0) {
                    *result = b;
                    return true;
                }
                break;
            default:
                break;
        }
    }

    // Rule 3: constant folding (both operands known)
    if (a_const && b_const) {
        double r = 0;
        bool ok = true;
        switch (op) {
            case XM_FADD:
                r = va + vb;
                break;
            case XM_FSUB:
                r = va - vb;
                break;
            case XM_FMUL:
                r = va * vb;
                break;
            case XM_FDIV:
                if (vb == 0.0) {
                    ok = false;
                    break;
                }
                r = va / vb;
                break;
            default:
                ok = false;
                break;
        }
        if (ok) {
            XmRef cref = xm_const_f64(func, r);
            *result = xm_emit_unary(func, blk, XM_CONST_F64, XR_REP_F64, cref);
            return true;
        }
        // Float comparison -> i64 result
        int64_t cr = 0;
        bool cmp_ok = true;
        switch (op) {
            case XM_FEQ:
                cr = (va == vb) ? 1 : 0;
                break;
            case XM_FNE:
                cr = (va != vb) ? 1 : 0;
                break;
            case XM_FLT:
                cr = (va < vb) ? 1 : 0;
                break;
            case XM_FLE:
                cr = (va <= vb) ? 1 : 0;
                break;
            default:
                cmp_ok = false;
                break;
        }
        if (cmp_ok) {
            XmRef cref = xm_const_i64(func, cr);
            *result = xm_emit_unary(func, blk, XM_CONST_I64, XR_REP_I64, cref);
            return true;
        }
    }

    return false;
}

/* ========== Unary fold rules ========== */

static bool fold_unary(XmFunc *func, uint16_t op, XmRef a, XmRef *result) {
    // Double negation: NEG(NEG(x)) → x
    if (op == XM_NEG && fold_def_op(func, a) == XM_NEG) {
        *result = fold_def_arg0(func, a);
        return !xm_ref_is_none(*result);
    }

    // Double bitwise NOT: NOT(NOT(x)) → x
    if (op == XM_NOT && fold_def_op(func, a) == XM_NOT) {
        *result = fold_def_arg0(func, a);
        return !xm_ref_is_none(*result);
    }

    // Double float negation: FNEG(FNEG(x)) → x
    if (op == XM_FNEG && fold_def_op(func, a) == XM_FNEG) {
        *result = fold_def_arg0(func, a);
        return !xm_ref_is_none(*result);
    }

    // Conversion roundtrip: F2I(I2F(x)) → x
    if (op == XM_F2I && fold_def_op(func, a) == XM_I2F) {
        *result = fold_def_arg0(func, a);
        return !xm_ref_is_none(*result);
    }

    /* Conversion roundtrip: I2F(F2I(x)) → x (lossy for non-integers, but
     * this pattern only appears when the original was integer) */
    if (op == XM_I2F && fold_def_op(func, a) == XM_F2I) {
        *result = fold_def_arg0(func, a);
        return !xm_ref_is_none(*result);
    }

    return false;
}

/* ========== Public API ========== */

XmRef xm_fold_emit(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a, XmRef b) {
    XR_DCHECK(func != NULL, "xm_fold_emit: NULL func");
    XR_DCHECK(blk != NULL, "xm_fold_emit: NULL blk");
    XmRef result;

    // Integer binary operations
    if (op >= XM_ADD && op <= XM_SHR) {
        if (fold_int_binary(func, blk, op, a, b, &result))
            return result;
    }

    // Float binary operations
    if ((op >= XM_FADD && op <= XM_FDIV) || (op >= XM_FEQ && op <= XM_FLE)) {
        if (fold_float_binary(func, blk, op, a, b, type, &result))
            return result;
    }

    // Integer comparison
    if (op >= XM_EQ && op <= XM_GE) {
        if (fold_int_binary(func, blk, op, a, b, &result))
            return result;
    }

    // Unary operations (NEG, NOT, FNEG, I2F, F2I)
    if (op == XM_NEG || op == XM_NOT || op == XM_FNEG || op == XM_I2F || op == XM_F2I) {
        if (fold_unary(func, op, a, &result))
            return result;
    }

    // No rule matched — emit normally
    return xm_emit(func, blk, op, type, a, b);
}

XmRef xm_fold_emit_unary(XmFunc *func, XmBlock *blk, uint16_t op, uint8_t type, XmRef a) {
    XR_DCHECK(func != NULL, "xm_fold_emit_unary: NULL func");
    XR_DCHECK(blk != NULL, "xm_fold_emit_unary: NULL blk");
    return xm_fold_emit(func, blk, op, type, a, XM_NONE);
}
