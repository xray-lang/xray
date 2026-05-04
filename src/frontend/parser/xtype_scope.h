/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_scope.h - Parser-owned type alias / generic parameter scope.
 *
 * KEY CONCEPT:
 *   The parser needs a tiny scoped name -> XrType* mapping while parsing
 *   `type Foo = ...` aliases and `fn foo<T>(...)` generic parameters.
 *   Historically it borrowed analyzer/XaScope for this, which forced a
 *   reverse dependency parser -> analyzer. This module replaces that with
 *   a self-contained scope that has zero analyzer coupling.
 *
 * SCALE:
 *   N is small per scope (<10 in practice). A simple linked list is faster
 *   than a hashmap up to ~16 entries and avoids extra allocations.
 */

#ifndef XTYPE_SCOPE_H
#define XTYPE_SCOPE_H

#include "../../base/xdefs.h"

// Forward declaration of the AST type reference (defined in xtype_ref.h).
typedef struct XrTypeRef XrTypeRef;

typedef struct XrTypeScope XrTypeScope;

// Single alias entry. `name` is heap-allocated and owned by the scope.
// `type` may be NULL during forward-declaration (e.g. `type A = A` self-ref
// guard); callers patch `type` directly once the RHS is parsed.
typedef struct XrTypeAlias {
    const char *name;
    XrTypeRef *type_ref;
    struct XrTypeAlias *next;
} XrTypeAlias;

// Allocate a new scope. `parent` may be NULL for the root scope.
XR_FUNC XrTypeScope *xr_type_scope_new(XrTypeScope *parent);

// Free a scope and all its alias entries (including their `name` strings).
// Does NOT touch `parent`. Safe to call with NULL.
XR_FUNC void xr_type_scope_free(XrTypeScope *scope);

// Define an alias in the innermost scope. Returns the new entry, or NULL if
// `name` is already defined locally (caller should report duplicate). The
// returned pointer is stable until the scope is freed; callers may mutate
// `entry->type` to patch forward-declared placeholders.
XR_FUNC XrTypeAlias *xr_type_scope_define(XrTypeScope *scope, const char *name, XrTypeRef *type_ref);

// Walk the scope chain to find an alias. Returns NULL if not found.
XR_FUNC XrTypeAlias *xr_type_scope_lookup(XrTypeScope *scope, const char *name);

// Lookup only in the innermost scope (no chain walk).
XR_FUNC XrTypeAlias *xr_type_scope_lookup_local(XrTypeScope *scope, const char *name);

// Convenience: walks the chain and returns just the type ref pointer (or NULL).
XR_FUNC XrTypeRef *xr_type_scope_resolve(XrTypeScope *scope, const char *name);

#endif  // XTYPE_SCOPE_H
