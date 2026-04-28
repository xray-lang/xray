/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_intrinsic.h - XIR intrinsic helper IDs
 *
 * KEY CONCEPT:
 *   When the XIR builder runs in AOT mode (b->aot_mode == true), it must
 *   not bake raw helper function pointers into the IR. Instead, it emits
 *   XIR_CALL_INTRINSIC with one of the IDs below as args[0] (encoded as a
 *   const i64). The AOT C codegen consults that ID directly, never the
 *   helper symbol name — that is what allows src/aot/ to be free of any
 *   reference to xr_jit_* / xrt_*_sentinel addresses.
 *
 *   JIT mode still emits XIR_CALL_C with raw fn_ptr; this enum is not
 *   used on that path.
 *
 * ADDING A NEW INTRINSIC:
 *   1. Add an XR_INTRIN_* enum value below.
 *   2. Update the xir_intrinsic_name() switch in xir.c (for trace dumps).
 *   3. Update xcgen_call.c::emit_call_intrinsic to lower it to C.
 *   4. Update the XIR builder site to emit XIR_CALL_INTRINSIC instead of
 *      XIR_CALL_C(fn=helper, ...) in aot_mode.
 *
 * RELATED MODULES:
 *   - src/jit/xir.h           : XIR_CALL_INTRINSIC opcode
 *   - src/jit/xir_builder*.c  : emission sites
 *   - src/aot/xcgen_call.c    : AOT lowering of XIR_CALL_INTRINSIC
 */

#ifndef XIR_INTRINSIC_H
#define XIR_INTRINSIC_H

#include <stdint.h>
#include "../base/xdefs.h"  /* XR_FUNC */

/* Forward-declare XIR types to avoid pulling in the full xir.h header. */
typedef struct XirFunc XirFunc;

typedef enum {
    XR_INTRIN_NONE = 0,

    /* --- Object/property access --- */
    XR_INTRIN_GETPROP = 1,    /* xr_jit_getprop */
    XR_INTRIN_INDEX_GET = 2,  /* xr_jit_index_get */
    XR_INTRIN_INDEX_SET = 3,  /* xr_jit_index_set */
    XR_INTRIN_TARRAY_GET = 4, /* xr_jit_tarray_get */
    XR_INTRIN_TARRAY_SET = 5, /* xr_jit_tarray_set */

    /* --- Map ops --- */
    XR_INTRIN_MAP_GET = 10,       /* xr_jit_map_get */
    XR_INTRIN_MAP_SET = 11,       /* xr_jit_map_set */
    XR_INTRIN_MAP_INCREMENT = 12, /* xr_jit_map_increment */

    /* --- StringBuilder (sentinels) --- */
    XR_INTRIN_STRBUF_NEW = 20,    /* xrt_strbuf_new_sentinel */
    XR_INTRIN_STRBUF_APPEND = 21, /* xrt_strbuf_append_sentinel */
    XR_INTRIN_STRBUF_FINISH = 22, /* xrt_strbuf_finish_sentinel */

    /* --- String ops --- */
    XR_INTRIN_SUBSTRING = 30,  /* xr_jit_substring */
    XR_INTRIN_STR_REPEAT = 31, /* xr_jit_str_repeat */
    XR_INTRIN_CHR = 32,        /* xr_jit_chr */

    /* --- Method dispatch --- */
    XR_INTRIN_INVOKE_METHOD = 40, /* xrt_invoke_method_sentinel */

    /* --- Shared variables --- */
    XR_INTRIN_GET_SHARED = 50, /* xr_jit_get_shared */
    XR_INTRIN_SET_SHARED = 51, /* xr_jit_set_shared */

    /* --- I/O --- */
    XR_INTRIN_PRINT = 60, /* xr_jit_print */

    /* --- Tagged arithmetic fallback --- */
    XR_INTRIN_RT_ADD = 70, /* xr_jit_rt_add */
    XR_INTRIN_RT_SUB = 71, /* xr_jit_rt_sub */
    XR_INTRIN_RT_MUL = 72, /* xr_jit_rt_mul */
    XR_INTRIN_RT_DIV = 73, /* xr_jit_rt_div */
    XR_INTRIN_RT_MOD = 74, /* xr_jit_rt_mod */

    /* --- Exception handling --- */
    XR_INTRIN_THROW = 80, /* xr_jit_throw */

    /* --- Json struct promotion --- */
    XR_INTRIN_JSON_NEW_SHAPE = 90, /* xr_json_new_with_shape */

    /* --- Type checking --- */
    XR_INTRIN_TYPEOF = 100, /* xr_jit_typeof */

    XR_INTRIN_COUNT, /* sentinel; keep last */
} XirIntrinsicId;

/* Returns a human-readable name for this intrinsic, or "intrin?" if unknown. */
XR_FUNC const char *xir_intrinsic_name(int id);

/*
 * Convert CALL_C/CALL_C_LEAF instructions whose fn_ptr matches a known
 * JIT helper into XIR_CALL_INTRINSIC(id, extra).  Must be called after
 * the optimizer pipeline and before AOT codegen.  JIT codegen never sees
 * XIR_CALL_INTRINSIC — this is AOT-only.
 */
XR_FUNC void xir_resolve_intrinsics(XirFunc *func);

#endif /* XIR_INTRINSIC_H */
