/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_pass_type.c - Type propagation, specialization, range analysis, redefines
 *
 * KEY CONCEPT:
 *   Multi-round type propagation refines vreg types from UNKNOWN to concrete
 *   (I64/F64/PTR/BOOL) based on defining instructions and guard assertions.
 *   Specialization replaces generic opcodes with type-specific fast paths.
 *   Range analysis detects induction variables for bounds-check elimination.
 */

#include "xm_pass_internal.h"
#include "xm_pass_limits.h"
#include "xm_domtree.h"
#include "xm_looptree.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype.h"

/*
 * Map xr_tag (XrValueTag) to canonical XrType*.
 * Used to narrow vreg type after GUARD_TAG succeeds.
 */
static XrType *xr_tag_to_xrtype(uint8_t tag) {
    switch (tag) {
        case 1:
        case 2:  // TRUE, FALSE
            return xr_type_new_bool(NULL);
        case 3:
        case 4:
        case 5:
        case 6:  // I8..I64
        case 7:
        case 8:
        case 9:
        case 10:  // U8..U64
            return xr_type_new_int(NULL);
        case 11:
        case 12:  // F32, F64
            return xr_type_new_float(NULL);
        case 13:          // PTR
            return NULL;  // PTR needs heap_type for precision
        default:
            return NULL;
    }
}

/*
 * Map XrType* to vtag (XrVRegTag) for compile-time type annotation.
 */
static uint8_t xrtype_to_vtag(XrType *t) {
    if (!t)
        return VTAG_TAGGED;
    switch (t->kind) {
        case XR_KIND_INT:
            return VTAG_I64;
        case XR_KIND_FLOAT:
            return VTAG_F64;
        case XR_KIND_BOOL:
            return VTAG_BOOL;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_CLASS:
        case XR_KIND_FUNCTION:
            return VTAG_PTR;
        case XR_KIND_UNION:
            return value_tag_to_vtag(xr_type_to_xr_tag(t));
        default:
            return VTAG_TAGGED;
    }
}

/* Set xrtype and vtag on a vreg. Allows refinement:
 * - TAGGED → any concrete tag (normal narrowing)
 * - PTR with unknown heap_type → PTR with known heap_type
 * - xrtype upgrade from NULL to non-NULL */
static void set_vreg_type(XmFunc *func, uint32_t vi, XrType *t, uint8_t vtag) {
    if (vi >= func->nvreg)
        return;
    if (!func->vregs[vi].xrtype)
        func->vregs[vi].xrtype = t;
    refine_vreg_vtag(func, vi, vtag);
}

/* Set heap_type on a vreg (for ALLOC, GUARD_CLASS, etc.).
 * Only sets if current heap_type is 0 (unknown). */
static void set_vreg_heap_type(XmFunc *func, uint32_t vi, uint16_t ht) {
    if (vi >= func->nvreg)
        return;
    if (func->vregs[vi].heap_type == 0) {
        func->vregs[vi].heap_type = ht;
        // Sync heap_cid on defining instruction
        XmIns *def = func->vregs[vi].def;
        if (def && def->ctype.heap_cid == 0)
            def->ctype.heap_cid = ht;
    }
}

/* Compute the meet (join) of two vtags in the type lattice.
 *   same → same
 *   I64 + F64 → NUMERIC (runtime check needed)
 *   PTR + PTR → PTR (different heap types, but both pointers)
 *   concrete + TAGGED → TAGGED
 *   anything else → TAGGED */
static uint8_t meet_vtag(uint8_t a, uint8_t b) {
    if (a == b)
        return a;
    // int + float → numeric union
    if ((a == VTAG_I64 && b == VTAG_F64) || (a == VTAG_F64 && b == VTAG_I64))
        return VTAG_NUMERIC;
    // numeric + int/float → numeric
    if ((a == VTAG_NUMERIC && (b == VTAG_I64 || b == VTAG_F64)) ||
        (b == VTAG_NUMERIC && (a == VTAG_I64 || a == VTAG_F64)))
        return VTAG_NUMERIC;
    // ptr + ptr → ptr (different heap types widen)
    if (a == VTAG_PTR && b == VTAG_PTR)
        return VTAG_PTR;
    // everything else falls to tagged
    return VTAG_TAGGED;
}

/*
 * xm_pass_type_prop: forward pass that propagates type information.
 *
 * Covers:
 *   - Constants (I64, F64, PTR)
 *   - Guards (GUARD_TAG, GUARD_NONNULL, GUARD_CLASS)
 *   - Arithmetic (int, float, mixed-type RT_*)
 *   - Comparisons (always bool)
 *   - Calls (CALL_KNOWN, CALL_KNOWN_REG, CALL_DIRECT, CALL_SELF_DIRECT)
 *   - Memory loads (LOAD_CORO, LOAD_FIELD, typed loads)
 *   - Runtime helpers (RT_ARRAY_NEW/LEN, RT_MAP_NEW, RT_ISNULL, etc.)
 *   - BOX/UNBOX, type conversions, MOV, NEG, SELECT
 *   - PHI meet with type lattice widening
 *   - ALLOC with heap_type extraction
 */
/* One forward scan of the type lattice.  Wrapped by xm_pass_type_prop
 * below which iterates until no vreg type changes. */
static void type_prop_scan_once(XmFunc *func) {
    XrType *t_int = xr_type_new_int(NULL);
    XrType *t_float = xr_type_new_float(NULL);
    XrType *t_bool = xr_type_new_bool(NULL);

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        /* PHI type meet with lattice widening:
         *   all same → propagate
         *   int + float → NUMERIC
         *   PTR + PTR (different heap_type) → PTR
         *   any TAGGED input → give up */
        for (XmPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xm_ref_is_vreg(phi->dst))
                continue;
            uint32_t dvi = XM_REF_INDEX(phi->dst);
            if (dvi >= func->nvreg)
                continue;
            if (xm_ref_ctype(func, phi->dst).kind != XM_TK_UNKNOWN)
                continue;
            if (phi->narg == 0)
                continue;

            uint8_t result_vtag = VTAG_TAGGED;
            bool valid = true;

            for (uint16_t a = 0; a < phi->narg; a++) {
                if (!xm_ref_is_vreg(phi->args[a])) {
                    valid = false;
                    break;
                }
                uint32_t ai = XM_REF_INDEX(phi->args[a]);
                if (ai >= func->nvreg) {
                    valid = false;
                    break;
                }
                uint8_t atag = type_kind_to_vtag(xm_ref_ctype(func, phi->args[a]).kind);
                if (atag == VTAG_TAGGED) {
                    valid = false;
                    break;
                }

                if (a == 0) {
                    result_vtag = atag;
                } else {
                    result_vtag = meet_vtag(result_vtag, atag);
                    if (result_vtag == VTAG_TAGGED) {
                        valid = false;
                        break;
                    }
                }
            }

            if (valid && result_vtag != VTAG_TAGGED) {
                force_vreg_vtag(func, dvi, result_vtag);
                // Set xrtype for uniform cases
                if (!func->vregs[dvi].xrtype) {
                    switch (result_vtag) {
                        case VTAG_I64:
                            func->vregs[dvi].xrtype = t_int;
                            break;
                        case VTAG_F64:
                            func->vregs[dvi].xrtype = t_float;
                            break;
                        case VTAG_BOOL:
                            func->vregs[dvi].xrtype = t_bool;
                            break;
                        default:
                            break;
                    }
                }
            }
        }

        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];

            switch (ins->op) {
                // ---- Constants ----
                case XM_CONST_I64:
                    if (xm_ref_is_vreg(ins->dst)) {
                        uint32_t vi = XM_REF_INDEX(ins->dst);
                        set_vreg_type(func, vi, t_int, VTAG_I64);
                    }
                    break;

                case XM_CONST_F64:
                    if (xm_ref_is_vreg(ins->dst)) {
                        uint32_t vi = XM_REF_INDEX(ins->dst);
                        set_vreg_type(func, vi, t_float, VTAG_F64);
                    }
                    break;

                case XM_CONST_PTR:
                    if (xm_ref_is_vreg(ins->dst)) {
                        uint32_t vi = XM_REF_INDEX(ins->dst);
                        refine_vreg_vtag(func, vi, VTAG_PTR);
                    }
                    break;

                // ---- Guards: narrow guarded vreg ----
                case XM_GUARD_TAG: {
                    if (!xm_ref_is_vreg(ins->args[0]))
                        break;
                    if (!xm_ref_is_const(ins->args[1]))
                        break;
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    if (ci >= func->nconst)
                        break;
                    uint8_t expected_tag = (uint8_t) func->consts[ci].val.raw;
                    XrType *narrowed = xr_tag_to_xrtype(expected_tag);
                    uint32_t vi = XM_REF_INDEX(ins->args[0]);
                    if (vi < func->nvreg) {
                        if (narrowed && !func->vregs[vi].xrtype)
                            func->vregs[vi].xrtype = narrowed;
                        refine_vreg_vtag(func, vi, value_tag_to_vtag(expected_tag));
                    }
                    break;
                }

                case XM_GUARD_NONNULL: {
                    if (!xm_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t vi = XM_REF_INDEX(ins->args[0]);
                    refine_vreg_vtag(func, vi, VTAG_PTR);
                    break;
                }

                case XM_GUARD_CLASS: {
                    if (!xm_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t vi = XM_REF_INDEX(ins->args[0]);
                    if (vi < func->nvreg) {
                        refine_vreg_vtag(func, vi, VTAG_PTR);
                        // Extract heap_type from guard constant
                        if (xm_ref_is_const(ins->args[1])) {
                            uint32_t ci = XM_REF_INDEX(ins->args[1]);
                            if (ci < func->nconst) {
                                uint16_t ht = (uint16_t) func->consts[ci].val.raw;
                                set_vreg_heap_type(func, vi, ht);
                            }
                        }
                    }
                    break;
                }

                case XM_GUARD_KLASS: {
                    // After klass guard, receiver is known to be PTR (instance)
                    if (!xm_ref_is_vreg(ins->args[0]))
                        break;
                    uint32_t vi = XM_REF_INDEX(ins->args[0]);
                    if (vi < func->nvreg) {
                        refine_vreg_vtag(func, vi, VTAG_PTR);
                        set_vreg_heap_type(func, vi, XR_TINSTANCE);
                    }
                    break;
                }

                // ---- Integer arithmetic ----
                case XM_ADD:
                case XM_SUB:
                case XM_MUL:
                case XM_AND:
                case XM_OR:
                case XM_XOR:
                case XM_SHL:
                case XM_SHR: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg || func->vregs[dvi].xrtype)
                        break;
                    XrType *at = NULL, *bt = NULL;
                    if (xm_ref_is_vreg(ins->args[0])) {
                        uint32_t ai = XM_REF_INDEX(ins->args[0]);
                        if (ai < func->nvreg)
                            at = func->vregs[ai].xrtype;
                    }
                    if (xm_ref_is_vreg(ins->args[1])) {
                        uint32_t bi2 = XM_REF_INDEX(ins->args[1]);
                        if (bi2 < func->nvreg)
                            bt = func->vregs[bi2].xrtype;
                    }
                    if (at && bt) {
                        if (at == t_int && bt == t_int)
                            set_vreg_type(func, dvi, t_int, VTAG_I64);
                        else if (at->kind == XR_KIND_FLOAT || bt->kind == XR_KIND_FLOAT)
                            set_vreg_type(func, dvi, t_float, VTAG_F64);
                    } else if (at == t_int || bt == t_int) {
                        set_vreg_type(func, dvi, t_int, VTAG_I64);
                    }
                    break;
                }

                case XM_NOT: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg || func->vregs[dvi].xrtype)
                        break;
                    // Bitwise NOT preserves int type
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                    break;
                }

                case XM_DIV:
                case XM_MOD: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg || func->vregs[dvi].xrtype)
                        break;
                    if (func->vregs[dvi].rep == XR_REP_F64)
                        set_vreg_type(func, dvi, t_float, VTAG_F64);
                    else if (func->vregs[dvi].rep == XR_REP_I64)
                        set_vreg_type(func, dvi, t_int, VTAG_I64);
                    break;
                }

                // ---- Comparisons: always bool ----
                case XM_EQ:
                case XM_NE:
                case XM_LT:
                case XM_LE:
                case XM_GT:
                case XM_GE:
                case XM_FEQ:
                case XM_FNE:
                case XM_FLT:
                case XM_FLE: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    set_vreg_type(func, dvi, t_bool, VTAG_BOOL);
                    break;
                }

                // ---- Mixed-type runtime comparisons: always bool ----
                case XM_RT_LT:
                case XM_RT_LE:
                case XM_RT_EQ: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    set_vreg_type(func, dvi, t_bool, VTAG_BOOL);
                    break;
                }

                // ---- Mixed-type runtime arithmetic: numeric result ----
                case XM_RT_ADD:
                case XM_RT_SUB:
                case XM_RT_MUL:
                case XM_RT_DIV:
                case XM_RT_MOD: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg || func->vregs[dvi].xrtype)
                        break;
                    // Infer from operand types if possible
                    uint8_t atag = type_kind_to_vtag(xm_ref_ctype(func, ins->args[0]).kind);
                    uint8_t btag = type_kind_to_vtag(xm_ref_ctype(func, ins->args[1]).kind);
                    if (atag == VTAG_I64 && btag == VTAG_I64 && ins->op != XM_RT_DIV) {
                        set_vreg_type(func, dvi, t_int, VTAG_I64);
                    } else if (atag == VTAG_F64 || btag == VTAG_F64) {
                        set_vreg_type(func, dvi, t_float, VTAG_F64);
                    } else if ((atag == VTAG_I64 || atag == VTAG_F64 || atag == VTAG_NUMERIC) &&
                               (btag == VTAG_I64 || btag == VTAG_F64 || btag == VTAG_NUMERIC)) {
                        // Both numeric but mixed → numeric union
                        refine_vreg_vtag(func, dvi, VTAG_NUMERIC);
                    }
                    break;
                }

                case XM_RT_UNM: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg || func->vregs[dvi].xrtype)
                        break;
                    // Unary minus preserves operand type
                    if (xm_ref_is_vreg(ins->args[0])) {
                        uint32_t ai = XM_REF_INDEX(ins->args[0]);
                        if (ai < func->nvreg && func->vregs[ai].xrtype)
                            set_vreg_type(func, dvi, func->vregs[ai].xrtype,
                                          type_kind_to_vtag(xm_ref_ctype(func, ins->args[0]).kind));
                    }
                    break;
                }

                // ---- Runtime array/collection helpers ----
                case XM_RT_ARRAY_NEW:
                case XM_RT_MAP_NEW: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    refine_vreg_vtag(func, dvi, VTAG_PTR);
                    set_vreg_heap_type(func, dvi, ins->op == XM_RT_ARRAY_NEW ? XR_TARRAY : XR_TMAP);
                    break;
                }

                case XM_RT_ARRAY_LEN: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                    break;
                }

                case XM_RT_ISNULL: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_bool, VTAG_BOOL);
                    break;
                }

                // ---- Calls with known return type ----
                case XM_CALL_KNOWN:
                case XM_CALL_KNOWN_REG: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    if (!xm_ref_is_const(ins->args[0]))
                        break;
                    uint32_t ci = XM_REF_INDEX(ins->args[0]);
                    if (ci >= func->nconst)
                        break;
                    XrProto *callee = (XrProto *) func->consts[ci].val.ptr;
                    if (callee && callee->return_type_info) {
                        XrType *rt = callee->return_type_info;
                        if (!func->vregs[dvi].xrtype)
                            func->vregs[dvi].xrtype = rt;
                        uint8_t inferred = xrtype_to_vtag(rt);
                        if (inferred != VTAG_TAGGED)
                            refine_vreg_vtag(func, dvi, inferred);
                    }
                    break;
                }

                case XM_CALL_SELF_DIRECT:
                case XM_CALL_DIRECT: {
                    // callee_proto stored on dst vreg by builder
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    XrProto *cp = func->vregs[dvi].callee_proto;
                    if (cp && cp->return_type_info) {
                        XrType *rt = cp->return_type_info;
                        if (!func->vregs[dvi].xrtype)
                            func->vregs[dvi].xrtype = rt;
                        uint8_t inferred = xrtype_to_vtag(rt);
                        if (inferred != VTAG_TAGGED)
                            refine_vreg_vtag(func, dvi, inferred);
                    }
                    break;
                }

                case XM_CALL_C: {
                    // C calls: infer from dst rep when possible
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    // If codegen assigned a specific rep, trust it
                    if (ins->ctype.kind == XM_TK_UNKNOWN) {
                        uint8_t rv = VTAG_TAGGED;
                        switch (func->vregs[dvi].rep) {
                            case XR_REP_I64:
                                rv = VTAG_I64;
                                break;
                            case XR_REP_F64:
                                rv = VTAG_F64;
                                break;
                            case XR_REP_PTR:
                                rv = VTAG_PTR;
                                break;
                            default:
                                break;
                        }
                        if (rv != VTAG_TAGGED)
                            refine_vreg_vtag(func, dvi, rv);
                    }
                    break;
                }

                // ---- Unary: NEG, FNEG ----
                case XM_NEG: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg || func->vregs[dvi].xrtype)
                        break;
                    if (xm_ref_is_vreg(ins->args[0])) {
                        uint32_t ai = XM_REF_INDEX(ins->args[0]);
                        if (ai < func->nvreg && func->vregs[ai].xrtype)
                            set_vreg_type(func, dvi, func->vregs[ai].xrtype,
                                          type_kind_to_vtag(xm_ref_ctype(func, ins->args[0]).kind));
                    }
                    break;
                }

                // ---- MOV: propagate type + tag ----
                case XM_MOV: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    if (xm_ref_is_vreg(ins->args[0])) {
                        uint32_t ai = XM_REF_INDEX(ins->args[0]);
                        if (ai < func->nvreg) {
                            if (!func->vregs[dvi].xrtype && func->vregs[ai].xrtype)
                                func->vregs[dvi].xrtype = func->vregs[ai].xrtype;
                            uint8_t ak = type_kind_to_vtag(xm_ref_ctype(func, ins->args[0]).kind);
                            if (ak != VTAG_TAGGED)
                                refine_vreg_vtag(func, dvi, ak);
                            // Propagate heap_type
                            if (func->vregs[dvi].heap_type == 0 && func->vregs[ai].heap_type != 0)
                                func->vregs[dvi].heap_type = func->vregs[ai].heap_type;
                        }
                    }
                    break;
                }

                // ---- SELECT: meet of true/false branches ----
                case XM_SELECT: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    if (ins->ctype.kind != XM_TK_UNKNOWN)
                        break;
                    uint8_t atag = type_kind_to_vtag(xm_ref_ctype(func, ins->args[0]).kind);
                    uint8_t btag = type_kind_to_vtag(xm_ref_ctype(func, ins->args[1]).kind);
                    if (atag != VTAG_TAGGED && btag != VTAG_TAGGED) {
                        uint8_t mt = meet_vtag(atag, btag);
                        if (mt != VTAG_TAGGED)
                            refine_vreg_vtag(func, dvi, mt);
                    }
                    break;
                }

                // ---- UNBOX: known result type ----
                case XM_UNBOX_I64: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                    break;
                }
                case XM_UNBOX_F64: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_float, VTAG_F64);
                    break;
                }

                // ---- BOX: propagate source tag ----
                case XM_BOX_I64: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    refine_vreg_vtag(func, dvi, VTAG_I64);
                    break;
                }
                case XM_BOX_F64: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    refine_vreg_vtag(func, dvi, VTAG_F64);
                    break;
                }

                // ---- Float arithmetic: always float ----
                case XM_FADD:
                case XM_FSUB:
                case XM_FMUL:
                case XM_FDIV:
                case XM_FNEG: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_float, VTAG_F64);
                    break;
                }

                // ---- Type conversions ----
                case XM_I2F: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_float, VTAG_F64);
                    break;
                }
                case XM_F2I: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                    break;
                }

                // ---- ALLOC: PTR + extract heap_type from packed constant ----
                case XM_ALLOC: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    if (ins->ctype.kind == XM_TK_UNKNOWN) {
                        ins->ctype.kind = XM_TK_PTR;
                    }
                    // Extract gc_type from args[0] packed constant:
                    // packed = (gc_extra << 8) | gc_type
                    if (xm_ref_is_const(ins->args[0])) {
                        uint32_t ci = XM_REF_INDEX(ins->args[0]);
                        if (ci < func->nconst) {
                            uint16_t gc_type = (uint16_t) (func->consts[ci].val.i64 & 0xFF);
                            set_vreg_heap_type(func, dvi, gc_type);
                        }
                    }
                    func->vregs[dvi].is_fresh_alloc = true;
                    break;
                }

                // ---- Memory loads: deterministic result types ----
                case XM_LOAD_CORO:
                case XM_LOAD_CORO_BYTE:
                case XM_LOAD32S:
                case XM_LOAD8Z:
                case XM_LOAD8S:
                case XM_LOAD16Z:
                case XM_LOAD16S:
                case XM_LOAD32Z:
                case XM_TAG_LOAD: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_int, VTAG_I64);
                    break;
                }

                case XM_LOAD_F32: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    set_vreg_type(func, dvi, t_float, VTAG_F64);
                    break;
                }

                // ---- LOAD_FIELD: infer from vreg rep ----
                case XM_LOAD_FIELD: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    if (ins->ctype.kind == XM_TK_UNKNOWN) {
                        uint8_t rv = VTAG_TAGGED;
                        switch (func->vregs[dvi].rep) {
                            case XR_REP_I64:
                                rv = VTAG_I64;
                                break;
                            case XR_REP_F64:
                                rv = VTAG_F64;
                                break;
                            case XR_REP_PTR:
                                rv = VTAG_PTR;
                                break;
                            default:
                                break;
                        }
                        if (rv != VTAG_TAGGED) {
                            ins->ctype.kind = vtag_to_type_kind(rv);
                        }
                    }
                    break;
                }

                // ---- RT_PRINT: void, skip ----
                case XM_RT_PRINT:
                    break;

                // ---- CATCH: exception object is always PTR ----
                case XM_CATCH: {
                    if (!xm_ref_is_vreg(ins->dst))
                        break;
                    uint32_t dvi = XM_REF_INDEX(ins->dst);
                    if (dvi >= func->nvreg)
                        break;
                    if (ins->ctype.kind == XM_TK_UNKNOWN)
                        ins->ctype.kind = XM_TK_PTR;
                    break;
                }

                default:
                    break;
            }
        }
    }
}

/*
 * FNV-1a hash of every vreg's (xrtype, ctype.kind, heap_type).  The
 * hash collapses to the same value iff no refinement was made during
 * the last scan, which lets xm_pass_type_prop detect convergence in
 * O(nvreg) without threading return values through hundreds of
 * setter call sites.
 */
static uint64_t type_prop_checksum(XmFunc *func) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v = 0; v < func->nvreg; v++) {
        uintptr_t t = (uintptr_t) func->vregs[v].xrtype;
        uint8_t k = 0;
        uint16_t ht = func->vregs[v].heap_type;
        if (func->vregs[v].def)
            k = (uint8_t) func->vregs[v].def->ctype.kind;
        h ^= (uint64_t) t;
        h *= 1099511628211ull;
        h ^= k;
        h *= 1099511628211ull;
        h ^= ht;
        h *= 1099511628211ull;
    }
    return h;
}

/* Worklist-style driver: iterate scan_once until the type state
 * stops changing.  In practice two or three rounds suffice even on
 * deeply chained IR; the cap guards against pathological input. */
#define TYPE_PROP_MAX_ROUNDS 6

XmPassChange xm_pass_type_prop(XmFunc *func) {
    if (!func || func->nblk == 0)
        return xm_pass_no_change();
    uint64_t initial = type_prop_checksum(func);
    for (int round = 0; round < TYPE_PROP_MAX_ROUNDS; round++) {
        uint64_t before = type_prop_checksum(func);
        type_prop_scan_once(func);
        if (type_prop_checksum(func) == before)
            break;
    }
    return type_prop_checksum(func) != initial ? (XmPassChange) {false, false, true, 0, 0, 0}
                                               : xm_pass_no_change();
}

/* ========== Type-Driven Specialization ========== */

/*
 * xm_pass_specialize: lower polymorphic RT_* instructions to monomorphic
 * native ops when type_prop has proven operand types.
 *
 * This runs after type_prop and converts:
 *   RT_LT/LE/EQ(I64,I64) → LT/LE/EQ   (native int compare)
 *   RT_LT/LE/EQ(F64,F64) → FLT/FLE/FEQ (native float compare)
 *   RT_ADD/SUB/MUL/DIV(F64,F64) → FADD/FSUB/FMUL/FDIV
 *
 * WHY THIS DESIGN:
 *   The builder emits RT_* when operand types are unknown at build time.
 *   After type_prop (which may run 3-5 times through the pipeline), operand
 *   types may be proven. Lowering to native ops enables subsequent passes
 *   (CSE, LICM, GVN, if-conversion) to optimize these operations, which
 *   they cannot do with opaque RT_* instructions.
 */
XmPassChange xm_pass_specialize(XmFunc *func) {
    if (!func || func->nblk == 0)
        return xm_pass_no_change();

    uint32_t n_spec = 0;
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            uint16_t orig_op = ins->op;

            // Get operand vtags for binary ops
            uint8_t ta = VTAG_TAGGED, tb = VTAG_TAGGED;
            uint8_t ra = XR_REP_TAGGED, rb = XR_REP_TAGGED;
            if (xm_ref_is_vreg(ins->args[0])) {
                uint32_t ai = XM_REF_INDEX(ins->args[0]);
                if (ai < func->nvreg) {
                    ta = type_kind_to_vtag(xm_ref_ctype(func, ins->args[0]).kind);
                    ra = func->vregs[ai].rep;
                }
            }
            if (xm_ref_is_vreg(ins->args[1])) {
                uint32_t bi2 = XM_REF_INDEX(ins->args[1]);
                if (bi2 < func->nvreg) {
                    tb = type_kind_to_vtag(xm_ref_ctype(func, ins->args[1]).kind);
                    rb = func->vregs[bi2].rep;
                }
            }

            switch (ins->op) {
                // ---- RT comparisons → native comparisons ----
                case XM_RT_LT:
                case XM_RT_LE:
                case XM_RT_EQ: {
                    if (ta == VTAG_I64 && tb == VTAG_I64 && ra == XR_REP_I64 && rb == XR_REP_I64) {
                        // Both proven I64: lower to native int compare
                        if (ins->op == XM_RT_LT)
                            ins->op = XM_LT;
                        else if (ins->op == XM_RT_LE)
                            ins->op = XM_LE;
                        else
                            ins->op = XM_EQ;
                        ins->rep = XR_REP_I64;
                        ins->flags &= ~XM_FLAG_SIDE_EFFECT;
                    } else if (ta == VTAG_F64 && tb == VTAG_F64 && ra == XR_REP_F64 &&
                               rb == XR_REP_F64) {
                        // Both proven F64: lower to native float compare
                        if (ins->op == XM_RT_LT)
                            ins->op = XM_FLT;
                        else if (ins->op == XM_RT_LE)
                            ins->op = XM_FLE;
                        else
                            ins->op = XM_FEQ;
                        ins->rep = XR_REP_I64;
                        ins->flags &= ~XM_FLAG_SIDE_EFFECT;
                    }
                    break;
                }

                // ---- RT arithmetic (both F64) → native float ops ----
                case XM_RT_ADD:
                case XM_RT_SUB:
                case XM_RT_MUL:
                case XM_RT_DIV: {
                    if (ra == XR_REP_F64 && rb == XR_REP_F64) {
                        switch (ins->op) {
                            case XM_RT_ADD:
                                ins->op = XM_FADD;
                                break;
                            case XM_RT_SUB:
                                ins->op = XM_FSUB;
                                break;
                            case XM_RT_MUL:
                                ins->op = XM_FMUL;
                                break;
                            case XM_RT_DIV:
                                ins->op = XM_FDIV;
                                break;
                            default:
                                break;
                        }
                        ins->rep = XR_REP_F64;
                        ins->flags &= ~XM_FLAG_SIDE_EFFECT;
                    }
                    break;
                }

                default:
                    break;
            }
            if (ins->op != orig_op)
                n_spec++;
        }
    }
    return n_spec ? (XmPassChange) {false, false, true, 0, 0, 0} : xm_pass_no_change();
}

/* ========== Write Barrier Elimination ========== */

/*
 * Per-block elimination of redundant write barriers.
 *
 * Two rules (inspired by Dart VM's EliminateWriteBarriers):
 *
 * Rule 1 — New allocation:
 *   If the parent object of BARRIER_FWD was allocated (XM_ALLOC) in the
 *   same block, and no GC-triggering instruction (SAFEPOINT, CALL_C,
 *   CALL_KNOWN) occurred between the allocation and the barrier, then
 *   the object is guaranteed to be in young space → no barrier needed.
 *
 * Rule 2 — Duplicate barrier:
 *   If the same parent object already has a BARRIER_FWD earlier in the
 *   block (with no intervening GC trigger), subsequent barriers on the
 *   same parent are redundant — the object is already in the remembered
 *   set from the first barrier.
 *
 * Both rule sets are invalidated by any instruction that can trigger GC,
 * since GC may promote young objects to old space or flush the store buffer.
 */
static bool can_trigger_gc(uint16_t op) {
    switch (op) {
        case XM_SAFEPOINT:
        case XM_CALL_C:
        case XM_CALL_KNOWN:
        case XM_ALLOC:
            return true;
        default:
            return false;
    }
}

XmPassChange xm_pass_elim_write_barriers(XmFunc *func) {
    if (!func)
        return xm_pass_no_change();

    uint32_t n_elim = 0;

/* Track up to 32 "known-safe" parent vregs per block.
 * A vreg is safe if it was freshly allocated (Rule 1) or
 * already had a barrier (Rule 2) since the last GC point. */
#define WBE_MAX_SAFE 32

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        XmRef safe_parents[WBE_MAX_SAFE];
        uint32_t nsafe = 0;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];

            // GC trigger: invalidate all tracked safe parents
            if (can_trigger_gc(ins->op)) {
                // But XM_ALLOC also defines a new young object
                if (ins->op == XM_ALLOC && xm_ref_is_vreg(ins->dst)) {
                    nsafe = 0;
                    if (nsafe < WBE_MAX_SAFE)
                        safe_parents[nsafe++] = ins->dst;
                } else {
                    nsafe = 0;
                }
                continue;
            }

            if (ins->op != XM_BARRIER_FWD)
                continue;

            // args[0] = parent object
            XmRef parent = ins->args[0];
            if (!xm_ref_is_vreg(parent))
                continue;

            // Check if parent is in safe set
            bool is_safe = false;
            for (uint32_t s = 0; s < nsafe; s++) {
                if (safe_parents[s] == parent) {
                    is_safe = true;
                    break;
                }
            }

            if (is_safe) {
                // Eliminate this barrier
                n_elim++;
                ins->op = XM_NOP;
                ins->dst = XM_NONE;
                ins->args[0] = XM_NONE;
                ins->args[1] = XM_NONE;
                ins->flags = 0;
            } else {
                /* Not safe yet — add parent to safe set (Rule 2:
                 * after one barrier, object is in remembered set) */
                if (nsafe < WBE_MAX_SAFE)
                    safe_parents[nsafe++] = parent;
            }
        }
    }

#undef WBE_MAX_SAFE
    return n_elim ? (XmPassChange) {false, true, false, n_elim, 0, 0} : xm_pass_no_change();
}

/* ========== Range Analysis ========== */

/*
 * Range analysis for integer values, inspired by Dart's RangeAnalysis.
 *
 * Goals:
 *   1. Track loop induction variables: i=0; i<N; i++ → range [0, N-1]
 *   2. Eliminate redundant GUARD_BOUNDS where index is provably in [0, length)
 *   3. Propagate range constraints from GUARD_BOUNDS to dominated uses
 *   4. Symbolic bounds: track i < arr.length to eliminate GUARD_BOUNDS(i, len)
 *
 * Algorithm:
 *   Phase 1: Collect all integer definitions and GUARD_BOUNDS
 *   Phase 2: Detect simple induction variables (phi with init + stride)
 *   Phase 3: Forward propagate ranges (iterative to fixed point, max 4 rounds)
 *   Phase 4: Eliminate redundant GUARD_BOUNDS (constant + symbolic)
 */

#define RA_MAX_ROUNDS XM_RA_MAX_ROUNDS

typedef struct {
    int64_t lo;       // inclusive lower bound
    int64_t hi;       // inclusive upper bound (constant)
    XmRef sym_bound;  // symbolic strict upper bound: value < sym_bound
                      // XM_NONE if no symbolic bound
    bool known;       // true if range is valid
} XmRange;

// Try to get constant value from a vreg
static bool ra_get_const(XmFunc *func, XmRef ref, int64_t *out) {
    if (!xm_ref_is_vreg(ref))
        return false;
    uint32_t vi = XM_REF_INDEX(ref);
    if (vi >= func->nvreg)
        return false;
    XmIns *def = func->vregs[vi].def;
    if (!def || def->op != XM_CONST_I64)
        return false;
    if (!xm_ref_is_const(def->args[0]))
        return false;
    uint32_t ci = XM_REF_INDEX(def->args[0]);
    if (ci >= func->nconst)
        return false;
    *out = func->consts[ci].val.i64;
    return true;
}

/* Detect simple induction variable pattern:
 *   phi(init, update) where update = phi + stride
 * Returns true if detected, sets *init_val and *stride_val */
static bool ra_detect_induction(XmFunc *func, XmPhi *phi, int64_t *init_val, int64_t *stride_val) {
    if (!phi || phi->narg != 2)
        return false;
    if (!xm_ref_is_vreg(phi->dst))
        return false;

    // Try both orderings: (init, update) and (update, init)
    for (int order = 0; order < 2; order++) {
        XmRef init_ref = phi->args[order];
        XmRef update_ref = phi->args[1 - order];

        // init must be a constant
        int64_t init;
        if (!ra_get_const(func, init_ref, &init))
            continue;

        // update must be phi +/- constant
        if (!xm_ref_is_vreg(update_ref))
            continue;
        uint32_t ui = XM_REF_INDEX(update_ref);
        if (ui >= func->nvreg)
            continue;
        XmIns *udef = func->vregs[ui].def;
        if (!udef)
            continue;

        int64_t stride;
        if (udef->op == XM_ADD) {
            // update = a + b, one of which is phi, the other is const stride
            if (udef->args[0] == phi->dst) {
                if (!ra_get_const(func, udef->args[1], &stride))
                    continue;
            } else if (udef->args[1] == phi->dst) {
                if (!ra_get_const(func, udef->args[0], &stride))
                    continue;
            } else
                continue;
        } else if (udef->op == XM_SUB) {
            // update = phi - const
            if (udef->args[0] != phi->dst)
                continue;
            if (!ra_get_const(func, udef->args[1], &stride))
                continue;
            stride = -stride;
        } else
            continue;

        *init_val = init;
        *stride_val = stride;
        return true;
    }
    return false;
}

/* Find the loop bound for an induction variable by looking at BR conditions.
 * Pattern: BR(LT(iv, bound)) or BR(GE(iv, bound)) at a back-edge block.
 * Returns true if a constant or symbolic bound is found.
 * sym_ref receives the symbolic bound vreg (e.g., arr.length) when
 * the bound is not a constant. sym_ref is XM_NONE for constant bounds. */
static bool ra_find_loop_bound(XmFunc *func, XmBlock *header, XmRef iv_ref, int64_t *bound_val,
                               XmRef *sym_ref) {
    *sym_ref = XM_NONE;
    for (uint32_t pi = 0; pi < header->npred; pi++) {
        XmBlock *pred = header->preds[pi];
        if (!pred || pred->nins == 0)
            continue;
        if (pred->jmp.type != XM_BR)
            continue;

        for (int32_t ii = (int32_t) pred->nins - 1; ii >= 0; ii--) {
            XmIns *ins = &pred->ins[ii];

            if (ins->op == XM_LT || ins->op == XM_LE) {
                if (ins->args[0] == iv_ref) {
                    int64_t bv;
                    if (ra_get_const(func, ins->args[1], &bv)) {
                        *bound_val = (ins->op == XM_LT) ? bv - 1 : bv;
                        return true;
                    }
                    /* Non-constant bound: track as symbolic.
                     * iv < sym means iv in [lo, sym-1] symbolically. */
                    if (ins->op == XM_LT && xm_ref_is_vreg(ins->args[1])) {
                        *sym_ref = ins->args[1];
                        *bound_val = INT64_MAX;
                        return true;
                    }
                }
            } else if (ins->op == XM_GT || ins->op == XM_GE) {
                if (ins->args[1] == iv_ref) {
                    int64_t bv;
                    if (ra_get_const(func, ins->args[0], &bv)) {
                        *bound_val = (ins->op == XM_GT) ? bv - 1 : bv;
                        return true;
                    }
                }
            }
            if ((int32_t) pred->nins - 1 - ii > 3)
                break;
        }
    }
    return false;
}

XmPassChange xm_pass_range_analysis(XmFunc *func) {
    if (!func || func->nvreg == 0 || func->nblk == 0)
        return xm_pass_no_change();

    uint32_t nv = func->nvreg;
    if (nv > XM_MAX_FUNC_VREGS)
        return xm_pass_no_change();  // bail on very large functions

    // Phase 1: Initialize range info for all vregs
    XmRange *ranges = (XmRange *) xr_calloc(nv, sizeof(XmRange));
    if (!ranges)
        return xm_pass_no_change();

    // Set ranges for constants
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (!xm_ref_is_vreg(ins->dst))
                continue;
            uint32_t dvi = XM_REF_INDEX(ins->dst);
            if (dvi >= nv)
                continue;

            if (ins->op == XM_CONST_I64) {
                int64_t val;
                if (ra_get_const(func, ins->dst, &val)) {
                    ranges[dvi].lo = val;
                    ranges[dvi].hi = val;
                    ranges[dvi].sym_bound = XM_NONE;
                    ranges[dvi].known = true;
                }
            }
        }
    }

    // Phase 2: Detect induction variables and set their ranges
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        for (XmPhi *phi = blk->phis; phi; phi = phi->next) {
            if (!xm_ref_is_vreg(phi->dst))
                continue;
            uint32_t pvi = XM_REF_INDEX(phi->dst);
            if (pvi >= nv)
                continue;

            int64_t init, stride;
            if (ra_detect_induction(func, phi, &init, &stride)) {
                int64_t bound;
                XmRef sym_ref;
                if (ra_find_loop_bound(func, blk, phi->dst, &bound, &sym_ref)) {
                    if (stride > 0 && (sym_ref != XM_NONE || init <= bound)) {
                        ranges[pvi].lo = init;
                        ranges[pvi].hi = bound;
                        ranges[pvi].sym_bound = sym_ref;
                        ranges[pvi].known = true;
                    } else if (stride < 0 && sym_ref == XM_NONE && bound <= init) {
                        ranges[pvi].lo = bound;
                        ranges[pvi].hi = init;
                        ranges[pvi].sym_bound = XM_NONE;
                        ranges[pvi].known = true;
                    }
                } else if (stride > 0) {
                    ranges[pvi].lo = init;
                    ranges[pvi].hi = INT64_MAX;
                    ranges[pvi].sym_bound = XM_NONE;
                    ranges[pvi].known = true;
                }
            }
        }
    }

    /* Phase 3: Forward propagate ranges through simple arithmetic.
     * Iterate up to RA_MAX_ROUNDS until no new ranges are discovered. */
    for (int round = 0; round < RA_MAX_ROUNDS; round++) {
        bool changed = false;

        for (uint32_t bi = 0; bi < func->nblk; bi++) {
            XmBlock *blk = func->blocks[bi];
            if (!blk)
                continue;

            for (uint32_t i = 0; i < blk->nins; i++) {
                XmIns *ins = &blk->ins[i];
                if (!xm_ref_is_vreg(ins->dst))
                    continue;
                uint32_t dvi = XM_REF_INDEX(ins->dst);
                if (dvi >= nv || ranges[dvi].known)
                    continue;

                if (ins->rep != XR_REP_I64)
                    continue;

                XmRange r0 = {0}, r1 = {0};
                if (xm_ref_is_vreg(ins->args[0])) {
                    uint32_t ai = XM_REF_INDEX(ins->args[0]);
                    if (ai < nv)
                        r0 = ranges[ai];
                }
                if (xm_ref_is_vreg(ins->args[1])) {
                    uint32_t ai = XM_REF_INDEX(ins->args[1]);
                    if (ai < nv)
                        r1 = ranges[ai];
                }

                if (!r0.known || !r1.known)
                    continue;

                switch (ins->op) {
                    case XM_ADD:
                        if (r0.lo >= 0 && r1.lo >= 0 && r0.hi <= INT64_MAX - r1.hi) {
                            ranges[dvi].lo = r0.lo + r1.lo;
                            ranges[dvi].hi = r0.hi + r1.hi;
                            ranges[dvi].sym_bound = XM_NONE;
                            ranges[dvi].known = true;
                            changed = true;
                        }
                        break;
                    case XM_SUB:
                        if (r0.lo >= r1.hi && r0.lo >= 0) {
                            ranges[dvi].lo = r0.lo - r1.hi;
                            ranges[dvi].hi = r0.hi - r1.lo;
                            ranges[dvi].sym_bound = XM_NONE;
                            ranges[dvi].known = true;
                            changed = true;
                        }
                        break;
                    case XM_AND:
                        if (r0.lo >= 0 && r1.lo >= 0) {
                            ranges[dvi].lo = 0;
                            ranges[dvi].hi = r0.hi < r1.hi ? r0.hi : r1.hi;
                            ranges[dvi].sym_bound = XM_NONE;
                            ranges[dvi].known = true;
                            changed = true;
                        }
                        break;
                    case XM_MUL:
                        if (r0.lo >= 0 && r1.lo >= 0 && r0.hi <= 0x7FFFFFFF &&
                            r1.hi <= 0x7FFFFFFF) {
                            ranges[dvi].lo = r0.lo * r1.lo;
                            ranges[dvi].hi = r0.hi * r1.hi;
                            ranges[dvi].sym_bound = XM_NONE;
                            ranges[dvi].known = true;
                            changed = true;
                        }
                        break;
                    case XM_MOD:
                        if (r0.lo >= 0 && r1.lo > 0) {
                            ranges[dvi].lo = 0;
                            int64_t max_mod = r1.hi - 1;
                            ranges[dvi].hi = (r0.hi < max_mod) ? r0.hi : max_mod;
                            ranges[dvi].sym_bound = XM_NONE;
                            ranges[dvi].known = true;
                            changed = true;
                        }
                        break;
                    case XM_SHL:
                        if (r0.lo >= 0 && r1.lo >= 0 && r1.lo == r1.hi && r1.lo < 63 &&
                            r0.hi <= (INT64_MAX >> r1.lo)) {
                            ranges[dvi].lo = r0.lo << r1.lo;
                            ranges[dvi].hi = r0.hi << r1.lo;
                            ranges[dvi].sym_bound = XM_NONE;
                            ranges[dvi].known = true;
                            changed = true;
                        }
                        break;
                    case XM_SHR:
                        if (r0.lo >= 0 && r1.lo >= 0 && r1.lo == r1.hi && r1.lo < 64) {
                            ranges[dvi].lo = 0;
                            ranges[dvi].hi = r0.hi >> r1.lo;
                            ranges[dvi].sym_bound = XM_NONE;
                            ranges[dvi].known = true;
                            changed = true;
                        }
                        break;
                    default:
                        break;
                }
            }
        }
        if (!changed)
            break;
    }

    // Phase 4: Eliminate redundant GUARD_BOUNDS + constraint propagation
    /* GUARD_BOUNDS: deopt if (unsigned)args[0] >= (unsigned)args[1]
     * Equivalent to: deopt if index < 0 || index >= length
     * Safe to eliminate if:
     *   (a) Constant: 0 <= index.lo && index.hi < length.lo
     *   (b) Symbolic: index.lo >= 0 && index.sym_bound == length_ref
     *       (iv < len proved by loop comparison, same vreg as GUARD_BOUNDS len)
     *
     * Constraint propagation: after a surviving GUARD_BOUNDS(idx, len),
     * we know idx is in [0, len-1]. This helps eliminate subsequent
     * GUARD_BOUNDS on the same index in the same block. */
    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        for (uint32_t i = 0; i < blk->nins; i++) {
            XmIns *ins = &blk->ins[i];
            if (ins->op != XM_GUARD_BOUNDS)
                continue;

            XmRef idx_ref = ins->args[0];
            XmRef len_ref = ins->args[1];

            XmRange idx_range = {0};
            XmRange len_range = {0};

            if (xm_ref_is_vreg(idx_ref)) {
                uint32_t vi = XM_REF_INDEX(idx_ref);
                if (vi < nv)
                    idx_range = ranges[vi];
            }
            if (xm_ref_is_vreg(len_ref)) {
                uint32_t vi = XM_REF_INDEX(len_ref);
                if (vi < nv)
                    len_range = ranges[vi];
            }

            // (a) Constant elimination: index range provably within [0, length)
            if (idx_range.known && len_range.known && idx_range.lo >= 0 &&
                idx_range.hi < len_range.lo) {
                ins->op = XM_NOP;
                ins->dst = XM_NONE;
                ins->args[0] = XM_NONE;
                ins->args[1] = XM_NONE;
                ins->flags = 0;
                continue;
            }

            /* (b) Symbolic elimination: iv < sym_bound && sym_bound == len_ref
             * Pattern: for (i=0; i<arr.length; i++) arr[i] = ...
             * The IV i has sym_bound pointing to the arr.length vreg,
             * and GUARD_BOUNDS(i, arr.length) uses the same vreg. */
            if (idx_range.known && idx_range.lo >= 0 && idx_range.sym_bound != XM_NONE &&
                idx_range.sym_bound == len_ref) {
                ins->op = XM_NOP;
                ins->dst = XM_NONE;
                ins->args[0] = XM_NONE;
                ins->args[1] = XM_NONE;
                ins->flags = 0;
                continue;
            }

            /* Constraint propagation: after this guard passes,
             * index is in [0, len-1]. */
            if (xm_ref_is_vreg(idx_ref)) {
                uint32_t vi = XM_REF_INDEX(idx_ref);
                if (vi < nv) {
                    if (!ranges[vi].known || ranges[vi].lo < 0)
                        ranges[vi].lo = 0;
                    ranges[vi].known = true;
                    if (len_range.known && len_range.lo > 0) {
                        int64_t max_idx = len_range.lo - 1;
                        if (ranges[vi].hi > max_idx)
                            ranges[vi].hi = max_idx;
                    }
                    // Also propagate symbolic bound from len_ref
                    if (ranges[vi].sym_bound == XM_NONE)
                        ranges[vi].sym_bound = len_ref;
                }
            }
        }
    }

    xr_free(ranges);
    return (XmPassChange) {false, false, true, 0, 0, 0};
}

#undef RA_MAX_ROUNDS

/* ========== REDEFINE Insertion ========== */
/*
 * Insert XM_REDEFINE after each GUARD instruction to create a new SSA
 * value with the narrowed type. Rewrites uses of the guarded vreg in the
 * same block (after the guard) and in all dominated blocks.
 *
 * Before: GUARD_TAG v0, TAG_INT
 *         ... uses v0 ...
 * After:  GUARD_TAG v0, TAG_INT
 *         v1 = REDEFINE v0   [ctype = INT]
 *         ... uses v1 ...
 *
 * This makes type narrowing flow-sensitive: only uses after the guard
 * see the narrowed type. Uses before the guard keep the original type.
 */
XmPassChange xm_pass_insert_redefines(XmFunc *func) {
    if (!func || func->nblk == 0)
        return xm_pass_no_change();

    bool any_inserted = false;

    const XmDomTree *dt = xm_func_get_domtree(func);

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;

        // Process guards from END to START to avoid index shift issues
        for (int32_t ii = (int32_t) blk->nins - 1; ii >= 0; ii--) {
            XmIns *ins = &blk->ins[ii];

            // Determine guarded vreg and narrowed type
            XmRef guarded = XM_NONE;
            XmType narrowed = XM_TYPE_UNKNOWN;

            switch (ins->op) {
                case XM_GUARD_TAG: {
                    if (!xm_ref_is_vreg(ins->args[0]))
                        continue;
                    if (!xm_ref_is_const(ins->args[1]))
                        continue;
                    uint32_t ci = XM_REF_INDEX(ins->args[1]);
                    if (ci >= func->nconst)
                        continue;
                    uint8_t expected_tag = (uint8_t) func->consts[ci].val.raw;
                    guarded = ins->args[0];
                    narrowed.kind = vtag_to_type_kind(value_tag_to_vtag(expected_tag));
                    break;
                }
                case XM_GUARD_NONNULL:
                    if (!xm_ref_is_vreg(ins->args[0]))
                        continue;
                    guarded = ins->args[0];
                    narrowed.kind = XM_TK_PTR;
                    break;
                case XM_GUARD_CLASS: {
                    if (!xm_ref_is_vreg(ins->args[0]))
                        continue;
                    guarded = ins->args[0];
                    narrowed.kind = XM_TK_PTR;
                    if (xm_ref_is_const(ins->args[1])) {
                        uint32_t ci = XM_REF_INDEX(ins->args[1]);
                        if (ci < func->nconst)
                            narrowed.heap_cid = (uint16_t) func->consts[ci].val.raw;
                    }
                    break;
                }
                case XM_GUARD_KLASS:
                    if (!xm_ref_is_vreg(ins->args[0]))
                        continue;
                    guarded = ins->args[0];
                    narrowed.kind = XM_TK_PTR;
                    break;
                default:
                    continue;
            }

            if (xm_ref_is_none(guarded))
                continue;
            uint32_t gvi = XM_REF_INDEX(guarded);
            if (gvi >= func->nvreg)
                continue;

            // Skip if guarded vreg already has a concrete type (no narrowing needed)
            XmType gct = xm_ref_ctype(func, guarded);
            if (gct.kind != XM_TK_UNKNOWN && narrowed.kind == gct.kind)
                continue;

            // Create new vreg for REDEFINE result
            uint8_t rep = func->vregs[gvi].rep;
            XmRef new_ref = xm_new_vreg(func, rep);
            uint32_t nvi = XM_REF_INDEX(new_ref);
            if (nvi >= func->nvreg)
                continue;

            // Insert REDEFINE at position ii+1
            uint32_t insert_pos = (uint32_t) ii + 1;
            XmIns *redef = xm_block_insert_at(func, blk, insert_pos);
            if (!redef)
                continue;

            any_inserted = true;
            redef->op = XM_REDEFINE;
            redef->rep = rep;
            redef->flags = 0;
            redef->ctype = narrowed;
            redef->dst = new_ref;
            redef->args[0] = guarded;
            redef->args[1] = XM_NONE;

            // Link new vreg to defining instruction
            func->vregs[nvi].def = redef;
            func->vregs[nvi].heap_type = narrowed.heap_cid;

            // Rewrite uses of guarded vreg in same block AFTER the REDEFINE
            for (uint32_t j = insert_pos + 1; j < blk->nins; j++) {
                XmIns *use = &blk->ins[j];
                for (int a = 0; a < 2; a++) {
                    if (use->args[a] == guarded)
                        use->args[a] = new_ref;
                }
                // Don't rewrite dst — that would be a different def
            }
            // Also rewrite terminator arg
            if (blk->jmp.arg == guarded)
                blk->jmp.arg = new_ref;

            // Rewrite uses in dominated blocks
            if (dt) {
                for (uint32_t dbi = 0; dbi < func->nblk; dbi++) {
                    if (dbi == bi)
                        continue;
                    if (!xm_dom_covers(dt, bi, dbi))
                        continue;
                    XmBlock *dblk = func->blocks[dbi];
                    if (!dblk)
                        continue;
                    for (uint32_t j = 0; j < dblk->nins; j++) {
                        XmIns *use = &dblk->ins[j];
                        for (int a = 0; a < 2; a++) {
                            if (use->args[a] == guarded)
                                use->args[a] = new_ref;
                        }
                    }
                    if (dblk->jmp.arg == guarded)
                        dblk->jmp.arg = new_ref;
                    // Rewrite PHI args in dominated blocks
                    for (XmPhi *phi = dblk->phis; phi; phi = phi->next) {
                        for (uint16_t p = 0; p < phi->narg; p++) {
                            if (phi->args[p] == guarded)
                                phi->args[p] = new_ref;
                        }
                    }
                }
            }
        }
    }
    return any_inserted ? (XmPassChange) {false, true, true, 0, 0, 0} : xm_pass_no_change();
}
