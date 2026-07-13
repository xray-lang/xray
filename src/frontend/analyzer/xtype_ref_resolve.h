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
 * formatter, LSP).
 */

#ifndef XTYPE_REF_RESOLVE_H
#define XTYPE_REF_RESOLVE_H

#include "../../base/xdefs.h"
#include <stdbool.h>
#include <stdint.h>

struct XrVMRuntime;
struct XrTypeRef;
struct XrType;
struct XaAnalyzer;
struct AstNode;

/* Resolve a single XrTypeRef to its runtime XrType*.
 * Returns xr_type_new_unknown() on NULL input or unresolvable refs. XR_TREF_ERROR
 * resolves to the compiler-only recovery error type instead of unknown. */
XR_FUNC struct XrType *xr_tref_resolve(struct XrVMRuntime *X, const struct XrTypeRef *tref);

/* Analyzer-aware variant.
 *
 * For XR_TREF_NAMED refs that match a class symbol in the analyzer's global
 * scope, this returns the **canonical** XrType registered with that symbol —
 * preserving the inheritance chain (`instance.superclass` / `class_ref`)
 * needed for class upper-bound constraint checks like `<T: Animal>`.
 *
 * For all other ref kinds (or when the name isn't a known class), behaviour
 * is identical to xr_tref_resolve(): falls back to fresh-XrType construction.
 *
 * Use this whenever a constraint or generic type argument needs to participate
 * in subclass relationships; the bare xr_tref_resolve() loses inheritance
 * information because it always allocates a brand-new class XrType. */
XR_FUNC struct XrType *xr_tref_resolve_in_analyzer(struct XaAnalyzer *analyzer,
                                                   const struct XrTypeRef *tref);

/* Evaluate the integer subset allowed in compile-time constant contexts:
 * integer literals, const integer identifiers, grouping, unary +/-/~, and
 * arithmetic/bitwise binary operators. */
XR_FUNC bool xa_eval_const_int_expr(struct XaAnalyzer *analyzer, const struct AstNode *expr,
                                    int64_t *out_value, const char **out_error);

#endif  // XTYPE_REF_RESOLVE_H
