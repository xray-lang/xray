/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_ref_resolve.h - Resolve XrTypeRef (AST type syntax) to XrType*
 *
 * The parser produces XrTypeRef; the analyzer resolves each ref into the
 * corresponding runtime XrType* for downstream consumption (codegen,
 * JIT, formatter, LSP).
 */

#ifndef XTYPE_REF_RESOLVE_H
#define XTYPE_REF_RESOLVE_H

#include "../../base/xdefs.h"

struct XrayIsolate;
struct XrTypeRef;
struct XrType;

/* Resolve a single XrTypeRef to its runtime XrType*.
 * Returns xr_type_new_unknown() on NULL input or unresolvable refs. */
XR_FUNC struct XrType *xr_tref_resolve(struct XrayIsolate *X, const struct XrTypeRef *tref);

#endif  // XTYPE_REF_RESOLVE_H
