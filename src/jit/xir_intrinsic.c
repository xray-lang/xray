/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_intrinsic.c - XIR intrinsic resolution and naming
 *
 * KEY CONCEPT:
 *   The resolve pass converts XIR_CALL_C instructions that reference known
 *   JIT helper function pointers into XIR_CALL_INTRINSIC instructions with
 *   a symbolic XirIntrinsicId.  This decouples the AOT C codegen from
 *   JIT runtime symbols: codegen dispatches on the intrinsic ID alone,
 *   never comparing void* addresses.
 *
 *   The pass rewrites args[0] in-place: the fn_ptr const is replaced with
 *   a const_i64 holding the intrinsic ID.  args[1] (extra_arg) is unchanged.
 *
 * ADDING A NEW INTRINSIC:
 *   1. Add an XR_INTRIN_* value in xir_intrinsic.h.
 *   2. Add a {fn_ptr, id} entry to the intrinsic_map[] table below.
 *   3. Add the name string to xir_intrinsic_name() below.
 *   4. Add the lowering case in the AOT codegen (xi_cgen.c).
 */

#include "xir_intrinsic.h"
#include "xir.h"
#include "xir_ops.h"
#include "xir_jit_runtime.h"
#include "xir_sentinels.h"
#include "../runtime/object/xjson.h"
#include "../base/xchecks.h"
#include <stddef.h>

/* ========== fn_ptr → intrinsic ID mapping table ========== */

typedef struct {
    void *fn_ptr;
    int id;
} IntrinsicMapEntry;

/* Populated once at first call; relies on linker-resolved addresses. */
static IntrinsicMapEntry intrinsic_map[] = {
    /* Object / property access */
    {NULL, XR_INTRIN_GETPROP},    /* xr_jit_getprop     */
    {NULL, XR_INTRIN_INDEX_GET},  /* xr_jit_index_get   */
    {NULL, XR_INTRIN_INDEX_SET},  /* xr_jit_index_set   */
    {NULL, XR_INTRIN_TARRAY_GET}, /* xr_jit_tarray_get  */
    {NULL, XR_INTRIN_TARRAY_SET}, /* xr_jit_tarray_set  */

    /* Map ops */
    {NULL, XR_INTRIN_MAP_GET},       /* xr_jit_map_get       */
    {NULL, XR_INTRIN_MAP_SET},       /* xr_jit_map_set       */
    {NULL, XR_INTRIN_MAP_INCREMENT}, /* xr_jit_map_increment */

    /* StringBuilder (sentinels) */
    {NULL, XR_INTRIN_STRBUF_NEW},    /* xrt_strbuf_new_sentinel    */
    {NULL, XR_INTRIN_STRBUF_APPEND}, /* xrt_strbuf_append_sentinel */
    {NULL, XR_INTRIN_STRBUF_FINISH}, /* xrt_strbuf_finish_sentinel */

    /* String ops */
    {NULL, XR_INTRIN_SUBSTRING},  /* xr_jit_substring  */
    {NULL, XR_INTRIN_STR_REPEAT}, /* xr_jit_str_repeat */
    {NULL, XR_INTRIN_CHR},        /* xr_jit_chr        */

    /* Method dispatch */
    {NULL, XR_INTRIN_INVOKE_METHOD}, /* xrt_invoke_method_sentinel */

    /* Shared variables */
    {NULL, XR_INTRIN_GET_SHARED}, /* xr_jit_get_shared */
    {NULL, XR_INTRIN_SET_SHARED}, /* xr_jit_set_shared */

    /* I/O */
    {NULL, XR_INTRIN_PRINT}, /* xr_jit_print */

    /* Tagged arithmetic fallback */
    {NULL, XR_INTRIN_RT_ADD}, /* xr_jit_rt_add */
    {NULL, XR_INTRIN_RT_SUB}, /* xr_jit_rt_sub */
    {NULL, XR_INTRIN_RT_MUL}, /* xr_jit_rt_mul */
    {NULL, XR_INTRIN_RT_DIV}, /* xr_jit_rt_div */
    {NULL, XR_INTRIN_RT_MOD}, /* xr_jit_rt_mod */

    /* Exception handling */
    {NULL, XR_INTRIN_THROW}, /* xr_jit_throw */

    /* Json struct promotion */
    {NULL, XR_INTRIN_JSON_NEW_SHAPE}, /* xr_json_new_with_shape */

    /* Type checking */
    {NULL, XR_INTRIN_TYPEOF}, /* xr_jit_typeof */
};

#define INTRINSIC_MAP_COUNT (int) (sizeof(intrinsic_map) / sizeof(intrinsic_map[0]))

/* One-time init: fill fn_ptr slots from linker symbols.  Thread-safety is
 * not required — AOT compilation is single-threaded. */
static bool map_initialized = false;

static void init_intrinsic_map(void) {
    if (map_initialized)
        return;
    int i = 0;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_getprop;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_index_get;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_index_set;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_tarray_get;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_tarray_set;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_map_get;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_map_set;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_map_increment;
    intrinsic_map[i++].fn_ptr = (void *) xrt_strbuf_new_sentinel;
    intrinsic_map[i++].fn_ptr = (void *) xrt_strbuf_append_sentinel;
    intrinsic_map[i++].fn_ptr = (void *) xrt_strbuf_finish_sentinel;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_substring;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_str_repeat;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_chr;
    intrinsic_map[i++].fn_ptr = (void *) xrt_invoke_method_sentinel;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_get_shared;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_set_shared;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_print;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_rt_add;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_rt_sub;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_rt_mul;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_rt_div;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_rt_mod;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_throw;
    intrinsic_map[i++].fn_ptr = (void *) xr_json_new_with_shape;
    intrinsic_map[i++].fn_ptr = (void *) xr_jit_typeof;
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

XR_FUNC void xir_resolve_intrinsics(XirFunc *func) {
    XR_DCHECK(func != NULL, "xir_resolve_intrinsics: NULL func");
    init_intrinsic_map();

    for (uint32_t bi = 0; bi < func->nblk; bi++) {
        XirBlock *blk = func->blocks[bi];
        if (!blk)
            continue;
        for (uint32_t ii = 0; ii < blk->nins; ii++) {
            XirIns *ins = &blk->ins[ii];
            if (ins->op != XIR_CALL_C && ins->op != XIR_CALL_C_LEAF)
                continue;

            /* args[0] must be a const-pool entry (fn_ptr or already-converted ID). */
            if (!xir_ref_is_const(ins->args[0]))
                continue;
            uint32_t ci = XIR_REF_INDEX(ins->args[0]);
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
            ins->op = XIR_CALL_INTRINSIC;
        }
    }
}

/* ========== Intrinsic Name ========== */

XR_FUNC const char *xir_intrinsic_name(int id) {
    switch (id) {
        case XR_INTRIN_GETPROP:
            return "getprop";
        case XR_INTRIN_INDEX_GET:
            return "index_get";
        case XR_INTRIN_INDEX_SET:
            return "index_set";
        case XR_INTRIN_TARRAY_GET:
            return "tarray_get";
        case XR_INTRIN_TARRAY_SET:
            return "tarray_set";
        case XR_INTRIN_MAP_GET:
            return "map_get";
        case XR_INTRIN_MAP_SET:
            return "map_set";
        case XR_INTRIN_MAP_INCREMENT:
            return "map_increment";
        case XR_INTRIN_STRBUF_NEW:
            return "strbuf_new";
        case XR_INTRIN_STRBUF_APPEND:
            return "strbuf_append";
        case XR_INTRIN_STRBUF_FINISH:
            return "strbuf_finish";
        case XR_INTRIN_SUBSTRING:
            return "substring";
        case XR_INTRIN_STR_REPEAT:
            return "str_repeat";
        case XR_INTRIN_CHR:
            return "chr";
        case XR_INTRIN_INVOKE_METHOD:
            return "invoke_method";
        case XR_INTRIN_GET_SHARED:
            return "get_shared";
        case XR_INTRIN_SET_SHARED:
            return "set_shared";
        case XR_INTRIN_PRINT:
            return "print";
        case XR_INTRIN_RT_ADD:
            return "rt_add";
        case XR_INTRIN_RT_SUB:
            return "rt_sub";
        case XR_INTRIN_RT_MUL:
            return "rt_mul";
        case XR_INTRIN_RT_DIV:
            return "rt_div";
        case XR_INTRIN_RT_MOD:
            return "rt_mod";
        case XR_INTRIN_THROW:
            return "throw";
        case XR_INTRIN_JSON_NEW_SHAPE:
            return "json_new_shape";
        case XR_INTRIN_TYPEOF:
            return "typeof";
        default:
            return "intrin?";
    }
}
