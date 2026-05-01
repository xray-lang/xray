/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_fold.c - On-the-fly peephole optimization (FOLD engine)
 *
 * KEY CONCEPT:
 *   Applied at IR construction time inside xi_to_xir. Each call to
 *   xir_fold_emit() checks a table of algebraic rules before delegating
 *   to xir_emit(). When a rule matches, the instruction is eliminated
 *   and an existing XirRef is returned instead.
 *
 * WHY THIS DESIGN:
 *   - Zero-cost when no rule matches (single switch + few comparisons)
 *   - Reduces IR size early, benefiting all downstream passes
 *   - Complements (not replaces) existing post-hoc passes
 */

#include "xir_fold.h"
#include "../base/xchecks.h"
#include <string.h>

/* ========== Helper: query constant value of a ref ========== */

/*
 * If ref is a vreg whose defining instruction is CONST_I64 loading a
 * constant pool entry, return true and write the i64 value to *out.
 */
static bool fold_get_const_i64(XirFunc *func, XirRef ref, int64_t *out) {
    if (!xir_ref_is_vreg(ref))
        return false;
    uint32_t idx = XIR_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return false;
    XirIns *def = func->vregs[idx].def;
    if (!def || def->op != XIR_CONST_I64)
        return false;
    if (!xir_ref_is_const(def->args[0]))
        return false;
    uint32_t cidx = XIR_REF_INDEX(def->args[0]);
    if (cidx >= func->nconst)
        return false;
    *out = func->consts[cidx].val.i64;
    return true;
}

static bool fold_get_const_f64(XirFunc *func, XirRef ref, double *out) {
    if (!xir_ref_is_vreg(ref))
        return false;
    uint32_t idx = XIR_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return false;
    XirIns *def = func->vregs[idx].def;
    if (!def || def->op != XIR_CONST_F64)
        return false;
    if (!xir_ref_is_const(def->args[0]))
        return false;
    uint32_t cidx = XIR_REF_INDEX(def->args[0]);
    if (cidx >= func->nconst)
        return false;
    *out = func->consts[cidx].val.f64;
    return true;
}

/* ========== Helper: query the defining opcode of a vreg ========== */

static uint16_t fold_def_op(XirFunc *func, XirRef ref) {
    if (!xir_ref_is_vreg(ref))
        return XIR_NOP;
    uint32_t idx = XIR_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return XIR_NOP;
    XirIns *def = func->vregs[idx].def;
    return def ? def->op : XIR_NOP;
}

// Get the first argument of a vreg's defining instruction
static XirRef fold_def_arg0(XirFunc *func, XirRef ref) {
    if (!xir_ref_is_vreg(ref))
        return XIR_NONE;
    uint32_t idx = XIR_REF_INDEX(ref);
    if (idx >= func->nvreg)
        return XIR_NONE;
    XirIns *def = func->vregs[idx].def;
    return def ? def->args[0] : XIR_NONE;
}

/* ========== Integer binary fold rules ========== */

/*
 * Try to fold an integer binary operation.
 * Returns true if folded, with *result set to the replacement ref.
 */
static bool fold_int_binary(XirFunc *func, XirBlock *blk, uint16_t op, XirRef a, XirRef b,
                            XirRef *result) {
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
            case XIR_ADD:
            case XIR_SUB:
            case XIR_OR:
            case XIR_XOR:
            case XIR_SHL:
            case XIR_SHR:
                if (vb == 0) {
                    *result = a;
                    return true;
                }
                break;
            case XIR_MUL:
                if (vb == 1) {
                    *result = a;
                    return true;
                }
                if (vb == 0) {
                    XirRef cref = xir_const_i64(func, 0);
                    *result = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
                    return true;
                }
                break;
            case XIR_DIV:
                if (vb == 1) {
                    *result = a;
                    return true;
                }
                break;
            case XIR_AND:
                if (vb == -1) {
                    *result = a;
                    return true;
                }  // x & 0xFFFFFFFFFFFFFFFF
                if (vb == 0) {
                    XirRef cref = xir_const_i64(func, 0);
                    *result = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
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
            case XIR_ADD:
            case XIR_OR:
            case XIR_XOR:
                if (va == 0) {
                    *result = b;
                    return true;
                }
                break;
            case XIR_MUL:
                if (va == 1) {
                    *result = b;
                    return true;
                }
                if (va == 0) {
                    XirRef cref = xir_const_i64(func, 0);
                    *result = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
                    return true;
                }
                break;
            case XIR_AND:
                if (va == -1) {
                    *result = b;
                    return true;
                }
                if (va == 0) {
                    XirRef cref = xir_const_i64(func, 0);
                    *result = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
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
            case XIR_SUB:
            case XIR_XOR: {
                XirRef cref = xir_const_i64(func, 0);
                *result = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
                return true;
            }
            case XIR_AND:
            case XIR_OR:
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
            case XIR_ADD:
                r = va + vb;
                break;
            case XIR_SUB:
                r = va - vb;
                break;
            case XIR_MUL:
                r = va * vb;
                break;
            case XIR_DIV:
                if (vb == 0) {
                    ok = false;
                    break;
                }
                r = va / vb;
                break;
            case XIR_MOD:
                if (vb == 0) {
                    ok = false;
                    break;
                }
                r = va % vb;
                break;
            case XIR_AND:
                r = va & vb;
                break;
            case XIR_OR:
                r = va | vb;
                break;
            case XIR_XOR:
                r = va ^ vb;
                break;
            case XIR_SHL:
                r = va << (vb & 63);
                break;
            case XIR_SHR:
                r = (int64_t) ((uint64_t) va >> (vb & 63));
                break;
            case XIR_EQ:
                r = (va == vb) ? 1 : 0;
                break;
            case XIR_NE:
                r = (va != vb) ? 1 : 0;
                break;
            case XIR_LT:
                r = (va < vb) ? 1 : 0;
                break;
            case XIR_LE:
                r = (va <= vb) ? 1 : 0;
                break;
            case XIR_GT:
                r = (va > vb) ? 1 : 0;
                break;
            case XIR_GE:
                r = (va >= vb) ? 1 : 0;
                break;
            default:
                ok = false;
                break;
        }
        if (ok) {
            XirRef cref = xir_const_i64(func, r);
            *result = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
            return true;
        }
    }

    return false;
}

/* ========== Float binary fold rules ========== */

static bool fold_float_binary(XirFunc *func, XirBlock *blk, uint16_t op, XirRef a, XirRef b,
                              uint8_t type, XirRef *result) {
    (void) type;
    double va, vb;
    bool a_const = fold_get_const_f64(func, a, &va);
    bool b_const = fold_get_const_f64(func, b, &vb);

    // Rule 1: identity with right constant (before constant folding)
    if (b_const) {
        switch (op) {
            case XIR_FADD:
            case XIR_FSUB:
                if (vb == 0.0) {
                    *result = a;
                    return true;
                }
                break;
            case XIR_FMUL:
                if (vb == 1.0) {
                    *result = a;
                    return true;
                }
                break;
            case XIR_FDIV:
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
            case XIR_FADD:
                if (va == 0.0) {
                    *result = b;
                    return true;
                }
                break;
            case XIR_FMUL:
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
            case XIR_FADD:
                r = va + vb;
                break;
            case XIR_FSUB:
                r = va - vb;
                break;
            case XIR_FMUL:
                r = va * vb;
                break;
            case XIR_FDIV:
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
            XirRef cref = xir_const_f64(func, r);
            *result = xir_emit_unary(func, blk, XIR_CONST_F64, XR_REP_F64, cref);
            return true;
        }
        // Float comparison -> i64 result
        int64_t cr = 0;
        bool cmp_ok = true;
        switch (op) {
            case XIR_FEQ:
                cr = (va == vb) ? 1 : 0;
                break;
            case XIR_FNE:
                cr = (va != vb) ? 1 : 0;
                break;
            case XIR_FLT:
                cr = (va < vb) ? 1 : 0;
                break;
            case XIR_FLE:
                cr = (va <= vb) ? 1 : 0;
                break;
            default:
                cmp_ok = false;
                break;
        }
        if (cmp_ok) {
            XirRef cref = xir_const_i64(func, cr);
            *result = xir_emit_unary(func, blk, XIR_CONST_I64, XR_REP_I64, cref);
            return true;
        }
    }

    return false;
}

/* ========== Unary fold rules ========== */

static bool fold_unary(XirFunc *func, uint16_t op, XirRef a, XirRef *result) {
    // Double negation: NEG(NEG(x)) → x
    if (op == XIR_NEG && fold_def_op(func, a) == XIR_NEG) {
        *result = fold_def_arg0(func, a);
        return !xir_ref_is_none(*result);
    }

    // Double bitwise NOT: NOT(NOT(x)) → x
    if (op == XIR_NOT && fold_def_op(func, a) == XIR_NOT) {
        *result = fold_def_arg0(func, a);
        return !xir_ref_is_none(*result);
    }

    // Double float negation: FNEG(FNEG(x)) → x
    if (op == XIR_FNEG && fold_def_op(func, a) == XIR_FNEG) {
        *result = fold_def_arg0(func, a);
        return !xir_ref_is_none(*result);
    }

    // Conversion roundtrip: F2I(I2F(x)) → x
    if (op == XIR_F2I && fold_def_op(func, a) == XIR_I2F) {
        *result = fold_def_arg0(func, a);
        return !xir_ref_is_none(*result);
    }

    /* Conversion roundtrip: I2F(F2I(x)) → x (lossy for non-integers, but
     * this pattern only appears when the original was integer) */
    if (op == XIR_I2F && fold_def_op(func, a) == XIR_F2I) {
        *result = fold_def_arg0(func, a);
        return !xir_ref_is_none(*result);
    }

    return false;
}

/* ========== Public API ========== */

XirRef xir_fold_emit(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type, XirRef a, XirRef b) {
    XR_DCHECK(func != NULL, "xir_fold_emit: NULL func");
    XR_DCHECK(blk != NULL, "xir_fold_emit: NULL blk");
    XirRef result;

    // Integer binary operations
    if (op >= XIR_ADD && op <= XIR_SHR) {
        if (fold_int_binary(func, blk, op, a, b, &result))
            return result;
    }

    // Float binary operations
    if ((op >= XIR_FADD && op <= XIR_FDIV) || (op >= XIR_FEQ && op <= XIR_FLE)) {
        if (fold_float_binary(func, blk, op, a, b, type, &result))
            return result;
    }

    // Integer comparison
    if (op >= XIR_EQ && op <= XIR_GE) {
        if (fold_int_binary(func, blk, op, a, b, &result))
            return result;
    }

    // Unary operations (NEG, NOT, FNEG, I2F, F2I)
    if (op == XIR_NEG || op == XIR_NOT || op == XIR_FNEG || op == XIR_I2F || op == XIR_F2I) {
        if (fold_unary(func, op, a, &result))
            return result;
    }

    // No rule matched — emit normally
    return xir_emit(func, blk, op, type, a, b);
}

XirRef xir_fold_emit_unary(XirFunc *func, XirBlock *blk, uint16_t op, uint8_t type, XirRef a) {
    XR_DCHECK(func != NULL, "xir_fold_emit_unary: NULL func");
    XR_DCHECK(blk != NULL, "xir_fold_emit_unary: NULL blk");
    return xir_fold_emit(func, blk, op, type, a, XIR_NONE);
}
