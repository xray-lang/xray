/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_alias.h - Provenance-based alias analysis.
 *
 * KEY CONCEPT:
 *   For each vreg of reference kind, we classify its "origin": does
 *   it come from a fresh allocation (XM_ALLOC), a function parameter
 *   (no defining instruction), a module-level / global read, or
 *   somewhere unknown?
 *
 *   Passes like LICM, DSE, store_to_load, and escape analysis use
 *   this classification to distinguish "this store cannot possibly
 *   alias that load" from "we have no information, assume worst".
 *
 * CURRENT SCOPE:
 *   The API surface is finalised here and the implementation answers
 *   FRESH_ALLOC vs PARAM vs UNKNOWN by walking def instructions.
 *   GLOBAL is reserved for a follow-up pass that inspects CALL_KNOWN /
 *   GETSHARED helpers.
 */

#ifndef XM_ALIAS_H
#define XM_ALIAS_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XmFunc XmFunc;
typedef uint32_t XmRef;

typedef enum {
    XM_ALIAS_UNKNOWN,      // conservative default
    XM_ALIAS_FRESH_ALLOC,  // provenance is a non-escaped XM_ALLOC
    XM_ALIAS_PARAM,        // comes from an incoming parameter vreg
    XM_ALIAS_GLOBAL,       // comes from module-level / builtin reads
} XmAliasSource;

typedef struct {
    XmAliasSource source;
    XmRef origin;  // the ALLOC/PARAM ref, or 0 when unknown
} XmAliasInfo;

/*
 * Query provenance of |ref| (must be a vreg) in |func|.  Returns a
 * pointer into the cached XmAliasInfo table; the table is owned by
 * the function and must not be freed by callers.  Returns NULL only
 * when the ref is not a vreg or the function has no vregs.
 */
XR_FUNC const XmAliasInfo *xm_func_get_alias(XmFunc *func, XmRef vreg_ref);

/*
 * Invalidate the cached alias info.  Call from any pass that rewrites
 * vreg definitions in a way that changes their source (scalar
 * replacement, inlining, etc.).  XM_RUN_PASS also drops this cache
 * as part of XM_RESET_ANALYSIS.
 */
XR_FUNC void xm_func_invalidate_alias(XmFunc *func);

#endif  // XM_ALIAS_H
