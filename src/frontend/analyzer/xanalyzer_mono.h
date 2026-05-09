/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_mono.h - Monomorphization Pass for generic functions and classes
 *
 * KEY CONCEPT:
 *   Duck-typed generics: compile-time instantiation of generic code for each
 *   concrete type combination. Generates up to 3 versions per generic (I64,
 *   F64, PTR) by rep-sharing. No trait syntax needed.
 *
 * WHY THIS DESIGN:
 *   - Type erasure forces TAGGED (16B boxed) for all generic params
 *   - Monomorphization enables native types (I64/F64) in JIT and AOT
 *   - Duck-typing avoids trait declaration overhead for a scripting language
 */

#ifndef XANALYZER_MONO_H
#define XANALYZER_MONO_H

#include "../parser/xast_nodes.h"
#include "../parser/xtype_ref.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xdefs.h"

#define XR_MONO_MAX_DEPTH 8

/* ========== Name Mangling ========== */

// Generate mangled name for a monomorphized function/class.
// Result is heap-allocated; caller must free.
// Example: mangle("identity", [int_tref], 1) -> "identity$i64"
XR_FUNC char *xr_mono_mangle(const char *name, XrTypeRef **type_args, int count);

// Encode a single type ref into its mangled form.
// Returns static string (no allocation needed).
XR_FUNC const char *xr_mono_type_tag(XrTypeRef *t);

/* ========== AST Clone ========== */

// Deep-clone an AST subtree. All child nodes and strings are duplicated.
// type_map: if non-NULL, maps type param names to concrete types during clone.
// type_map_count: number of entries in type_map.
typedef struct {
    const char *param_name;  // Type parameter name (e.g., "T")
    XrTypeRef *concrete_type; // Concrete type ref to substitute
} XrMonoTypeMap;

XR_FUNC AstNode *xr_ast_clone(AstNode *node, XrMonoTypeMap *type_map, int type_map_count);

/* ========== Type Substitution ========== */

// Substitute type parameters in a type ref tree.
// Returns the type ref with all TYPE_PARAM kinds replaced by concrete types.
// If no substitution needed, may return the original type ref.
XR_FUNC XrTypeRef *xr_mono_type_substitute(XrTypeRef *type, XrMonoTypeMap *type_map, int type_map_count);

/* ========== Mono Instance Tracking ========== */

typedef struct {
    const char *generic_name;  // Original generic name
    XrTypeRef **type_args;     // Concrete type ref arguments
    int type_arg_count;
    const char *mangled_name;  // Mangled name (heap-allocated)
    uint32_t rep_signature;    // Combined slot-type signature for dedup
    bool is_class_generic;     // true for class/struct generics (disable rep-sharing)
} XaMonoInstance;

typedef struct {
    XaMonoInstance *instances;
    int count;
    int capacity;
} XaMonoCollector;

XR_FUNC void xa_mono_collector_init(XaMonoCollector *c);
XR_FUNC void xa_mono_collector_free(XaMonoCollector *c);

// Add a generic instantiation. Returns the mangled name (owned by collector).
// For class/struct generics (is_class_generic=true), dedup uses mangled name
// instead of rep-signature to avoid conflating distinct reference types.
XR_FUNC const char *xa_mono_collector_add(XaMonoCollector *c, const char *generic_name,
                                          XrTypeRef **type_args, int type_arg_count,
                                          bool is_class_generic);

/* ========== Mono Pass ========== */

// Run the full monomorphization pass on a program AST.
// Collects generic declarations and instantiation sites, clones+substitutes
// for each concrete type combination, injects into program, rewrites call sites.
// Safe to call on programs with no generics (no-op).
XR_FUNC void xa_mono_pass(AstNode *root);

#endif  // XANALYZER_MONO_H
