/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_alias.h - Provenance-based alias analysis.
 *
 * KEY CONCEPT:
 *   For each vreg of reference kind, we classify its "origin": does
 *   it come from a fresh allocation (XIR_ALLOC), a function parameter
 *   (no defining instruction), a module-level / global read, or
 *   somewhere unknown?
 *
 *   Passes like LICM, DSE, store_to_load, and escape analysis use
 *   this classification to distinguish "this store cannot possibly
 *   alias that load" from "we have no information, assume worst".
 *
 * SCOPE IN PHASE 2:
 *   The API surface is finalised here and a minimal implementation
 *   answers FRESH_ALLOC vs PARAM vs UNKNOWN by walking def
 *   instructions.  GLOBAL is reserved for a follow-up pass that
 *   inspects CALL_KNOWN / GETSHARED helpers.
 */

#ifndef XIR_ALIAS_H
#define XIR_ALIAS_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XirFunc XirFunc;
typedef uint32_t XirRef;

typedef enum {
    XIR_ALIAS_UNKNOWN,        // conservative default
    XIR_ALIAS_FRESH_ALLOC,    // provenance is a non-escaped XIR_ALLOC
    XIR_ALIAS_PARAM,          // comes from an incoming parameter vreg
    XIR_ALIAS_GLOBAL,         // comes from module-level / builtin reads
} XirAliasSource;

typedef struct {
    XirAliasSource source;
    XirRef         origin;    // the ALLOC/PARAM ref, or 0 when unknown
} XirAliasInfo;

/*
 * Query provenance of |ref| (must be a vreg) in |func|.  Returns a
 * pointer into the cached XirAliasInfo table; the table is owned by
 * the function and must not be freed by callers.  Returns NULL only
 * when the ref is not a vreg or the function has no vregs.
 */
XR_FUNC const XirAliasInfo *xir_func_get_alias(XirFunc *func, XirRef vreg_ref);

/*
 * Invalidate the cached alias info.  Call from any pass that rewrites
 * vreg definitions in a way that changes their source (scalar
 * replacement, inlining, etc.).  XIR_RUN_PASS also drops this cache
 * as part of XIR_RESET_ANALYSIS.
 */
XR_FUNC void xir_func_invalidate_alias(XirFunc *func);

#endif // XIR_ALIAS_H
