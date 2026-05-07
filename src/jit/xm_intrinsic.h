/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_intrinsic.h - Xm intrinsic helper IDs
 *
 * XM_CALL_INTRINSIC uses one of the IDs below as args[0] (encoded as a
 * const i64). The AOT C codegen (xi_cgen.c) consults that ID directly,
 * never the helper symbol name — that allows src/aot/ to be free of any
 * reference to xr_jit_* addresses.
 *
 * ADDING A NEW INTRINSIC:
 *   1. Add one XI_INTRINSIC() line in src/ir/xi_intrinsic.def.
 *   2. Implement the C helper in src/jit/ or src/aot/.
 *   3. Add the lowering case in the AOT codegen (xi_cgen.c).
 *   The enum, name table, and arity table are auto-generated from the .def.
 *
 * RELATED MODULES:
 *   - src/ir/xi_intrinsic.def : single source of truth for intrinsic IDs
 *   - src/jit/xm.h           : XM_CALL_INTRINSIC opcode
 *   - src/aot/xi_cgen.c       : AOT C code generation
 */

#ifndef XM_INTRINSIC_H
#define XM_INTRINSIC_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h" /* XR_FUNC */

/* Forward-declare Xm types to avoid pulling in the full xm.h header. */
typedef struct XmFunc XmFunc;

/* Generated from xi_intrinsic.def — do not edit by hand */
typedef enum {
    XR_INTRIN_NONE = 0,
#define XI_INTRINSIC(name, id, arity, helper) XR_INTRIN_##name = id,
#include "../ir/xi_intrinsic.def"
#undef XI_INTRINSIC
    XR_INTRIN_COUNT /* sentinel; keep last */
} XmIntrinsicId;

/* Returns a human-readable name for this intrinsic, or "intrin?" if unknown.
 * Generated from xi_intrinsic.def. */
XR_FUNC const char *xm_intrinsic_name(int id);

/* Returns the expected arity for this intrinsic, or -1 for variadic / unknown. */
XR_FUNC int xm_intrinsic_arity(int id);

/* Returns true if the given ID is a valid intrinsic. */
static inline bool xm_intrinsic_valid(int id) {
    return id > XR_INTRIN_NONE && id < XR_INTRIN_COUNT;
}

/*
 * Convert CALL_C/CALL_C_LEAF instructions whose fn_ptr matches a known
 * JIT helper into XM_CALL_INTRINSIC(id, extra).  Must be called after
 * the optimizer pipeline and before AOT codegen.  JIT codegen never sees
 * XM_CALL_INTRINSIC — this is AOT-only.
 */
XR_FUNC void xm_resolve_intrinsics(XmFunc *func);

#endif /* XM_INTRINSIC_H */
