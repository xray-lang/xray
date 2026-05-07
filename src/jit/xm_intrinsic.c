/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_intrinsic.c - Xm intrinsic resolution and naming
 *
 * KEY CONCEPT:
 *   The resolve pass converts XM_CALL_C instructions that reference known
 *   JIT helper function pointers into XM_CALL_INTRINSIC instructions with
 *   a symbolic XmIntrinsicId.  This decouples the AOT C codegen from
 *   JIT runtime symbols: codegen dispatches on the intrinsic ID alone,
 *   never comparing void* addresses.
 *
 *   The pass rewrites args[0] in-place: the fn_ptr const is replaced with
 *   a const_i64 holding the intrinsic ID.  args[1] (extra_arg) is unchanged.
 *
 * ADDING A NEW INTRINSIC:
 *   1. Add one XI_INTRINSIC() line in src/ir/xi_intrinsic.def.
 *   2. Add a {fn_ptr, id} entry to the intrinsic_map[] table below.
 *   3. Add the lowering case in the AOT codegen (xi_cgen.c).
 *   The enum, name table, and arity table are auto-generated from the .def.
 */

#include "xm_intrinsic.h"
#include "xm.h"
#include "xm_ops.h"
#include "xm_jit_runtime.h"
#include "xm_sentinels.h"
#include "../runtime/object/xjson.h"
#include "../ir/xi_intrinsic_flags.h"
#include "../base/xchecks.h"
#include <stddef.h>

/* ========== fn_ptr → intrinsic ID mapping table ========== */

typedef struct {
    void *fn_ptr;
    int id;
} IntrinsicMapEntry;

/* Auto-generated from xi_intrinsic.def — IDs and fn_ptrs in one table. */
static IntrinsicMapEntry intrinsic_map[] = {
#define XI_INTRINSIC(name, id, arity, helper, eff, rep) \
    {NULL, XR_INTRIN_##name},
#include "../ir/xi_intrinsic.def"
#undef XI_INTRINSIC
};

#define INTRINSIC_MAP_COUNT (int)(sizeof(intrinsic_map) / sizeof(intrinsic_map[0]))

/* One-time init: fill fn_ptr slots from linker symbols.
 * Auto-generated from xi_intrinsic.def — adding a new intrinsic
 * to the .def file automatically registers its fn_ptr here. */
static bool map_initialized = false;

static void init_intrinsic_map(void) {
    if (map_initialized) return;
    int i = 0;
#define XI_INTRINSIC(name, id, arity, helper, eff, rep) \
    intrinsic_map[i++].fn_ptr = (void *)(helper);
#include "../ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    XR_DCHECK(i == INTRINSIC_MAP_COUNT, "intrinsic map init count mismatch");
    map_initialized = true;
}

/* Linear scan is fine for ~26 entries; called once per CALL_C instruction
 * during the resolve pass (not on the hot path). */
static int lookup_intrinsic(void *fn_ptr) {
    for (int i = 0; i < INTRINSIC_MAP_COUNT; i++) {
        if (intrinsic_map[i].fn_ptr == fn_ptr)
            return intrinsic_map[i].id;
    }
    return XR_INTRIN_NONE;
}

/* ========== Resolution Pass ========== */

XR_FUNC void xm_resolve_intrinsics(XmFunc *func) {
    XR_DCHECK(func != NULL, "xm_resolve_intrinsics: NULL func");
    init_intrinsic_map();

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XmBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XmIns *ins = &blk->ins[ii];
            if (ins->op != XM_CALL_C && ins->op != XM_CALL_C_LEAF)
                continue;

            /* args[0] must be a const-pool entry (fn_ptr or already-converted ID). */
            if (!xm_ref_is_const(ins->args[0]))
                continue;
            uint32_t ci = XM_REF_INDEX(ins->args[0]);
            if (ci >= func->nconst)
                continue;

            int id = XR_INTRIN_NONE;
            if (func->consts[ci].rep == XR_REP_PTR) {
                /* First encounter: resolve fn_ptr → intrinsic ID. */
                void *fn_ptr = func->consts[ci].val.ptr;
                id = lookup_intrinsic(fn_ptr);
                if (id == XR_INTRIN_NONE)
                    continue;
                /* Rewrite const slot in-place (shared by all users of this ci). */
                func->consts[ci].rep = XR_REP_I64;
                func->consts[ci].val.i64 = (int64_t) id;
            } else if (func->consts[ci].rep == XR_REP_I64) {
                /* Already converted by a previous instruction sharing this const.
                 * Validate that it holds a valid intrinsic ID. */
                int64_t v = func->consts[ci].val.i64;
                if (v > XR_INTRIN_NONE && v < XR_INTRIN_COUNT)
                    id = (int) v;
                else
                    continue; /* not an intrinsic — unrelated i64 const */
            } else {
                continue;
            }

            /* Change opcode from CALL_C/CALL_C_LEAF to CALL_INTRINSIC.
             * Preserve all other fields (dst, args[1], flags). */
            ins->op = XM_CALL_INTRINSIC;
        }
    }
}

/* ========== Intrinsic Name / Arity (generated from xi_intrinsic.def) ========== */

/* Lowercase helper name for trace dumps.  Keep macro-generated names
 * lowercase by stringifying the enum suffix (matches existing output). */
XR_FUNC const char *xm_intrinsic_name(int id) {
    switch (id) {
#define XI_INTRINSIC(name, id_val, ar, helper, eff, rep) \
    case id_val: return #helper;
#include "../ir/xi_intrinsic.def"
#undef XI_INTRINSIC
        default: return "intrin?";
    }
}

XR_FUNC int xm_intrinsic_arity(int id) {
    switch (id) {
#define XI_INTRINSIC(name, id_val, ar, helper, eff, rep) \
    case id_val: return ar;
#include "../ir/xi_intrinsic.def"
#undef XI_INTRINSIC
        default: return -1;
    }
}

XR_FUNC int xm_intrinsic_effects(int id) {
    switch (id) {
#define XI_INTRINSIC(name, id_val, ar, helper, eff, rep) \
    case id_val: return (eff);
#include "../ir/xi_intrinsic.def"
#undef XI_INTRINSIC
        default: return 0;
    }
}

XR_FUNC int xm_intrinsic_ret_rep(int id) {
    switch (id) {
#define XI_INTRINSIC(name, id_val, ar, helper, eff, rep) \
    case id_val: return (rep);
#include "../ir/xi_intrinsic.def"
#undef XI_INTRINSIC
        default: return IREP_VAL;
    }
}
