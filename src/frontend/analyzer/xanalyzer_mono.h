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
 *   concrete type combination. The concrete type tuple defines instance
 *   identity -- the frontend never merges two distinct type arguments, so
 *   member resolution, field layout and debug type names stay exact. Code
 *   sharing between ABI-equivalent bodies is an AOT decision (see the
 *   generic-body-plan / generic-code-size-plan evidence rows), not a frontend
 *   one, because a duck-typed body resolves `x.foo()` against the concrete
 *   type argument and cannot be shared before that resolution is known.
 *
 * WHY THIS DESIGN:
 *   - Type erasure forces TAGGED (16B boxed) for all generic params
 *   - Monomorphization enables native types (I64/F64) in AOT
 *   - Duck-typing avoids trait declaration overhead for a scripting language
 */

#ifndef XANALYZER_MONO_H
#define XANALYZER_MONO_H

#include "../parser/xast_nodes.h"
#include "../parser/xtype_ref.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xforward_decl.h"
#include "../../base/xdefs.h"
#include "../../base/xlocation.h"

/* ========== Instantiation Budgets ==========
 *
 * Two budgets guard two different risks, and they are not interchangeable:
 *
 *   XR_MONO_MAX_DEPTH     bounds *nesting*. A specialized body may instantiate
 *                         further generics (Router<int> building RouteMatch<int>
 *                         building Entry<int>), so the expansion in
 *                         inject_mono_decls is a fixpoint. Polymorphic
 *                         recursion (`fn f<T>() { f<Box<T>>() }`) makes that
 *                         fixpoint diverge, and depth is the only thing that
 *                         can detect it: every round produces a genuinely new
 *                         type tuple, so no dedup or counter can distinguish
 *                         divergence from legitimate breadth. This budget is
 *                         load-bearing for termination.
 *
 *   XR_MONO_MAX_INSTANCES bounds *breadth*. Each instance clones a whole
 *                         declaration, so this is a compile-time memory
 *                         backstop, not a language rule. It is set far above
 *                         any plausible real program: a program that trips it
 *                         has a generic expansion problem worth seeing.
 *
 * Both are hard errors (E0388 / E0387). Exceeding a budget never silently
 * leaves a call unspecialized -- a silent fallback would reintroduce boxing
 * underneath an `xray verify` no-box contract that claims it cannot happen.
 */
#define XR_MONO_MAX_DEPTH 32
#define XR_MONO_MAX_INSTANCES 16384

typedef struct XaAnalyzer XaAnalyzer;

/* A generic higher-order function has one additional finite specialization
 * dimension.  MAY_THROW deliberately keeps the historical mangled name;
 * NO_THROW adds a suffix, so a concrete type tuple can produce at most two
 * executable bodies. */
typedef enum XaMonoThrowEffect {
    XA_MONO_EFFECT_NONE = 0,
    XA_MONO_EFFECT_MAY_THROW,
    XA_MONO_EFFECT_NO_THROW,
} XaMonoThrowEffect;

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
    const char *param_name;    // Type parameter name (e.g., "T")
    XrTypeRef *concrete_type;  // Concrete type ref to substitute
} XrMonoTypeMap;

XR_FUNC AstNode *xr_ast_clone(AstNode *node, XrMonoTypeMap *type_map, int type_map_count);

// Deep-clone an AST subtree, assigning fresh stable node ids from `session`.
// Required when the clone must coexist with the original in the analyzer's
// node-id-keyed side table (e.g. caller-side default argument completion).
XR_FUNC AstNode *xr_ast_clone_session(AstNode *node, struct XrCompilerSession *session);

/* ========== Type Substitution ========== */

// Substitute type parameters in a type ref tree.
// Returns the type ref with all TYPE_PARAM kinds replaced by concrete types.
// If no substitution needed, may return the original type ref.
XR_FUNC XrTypeRef *xr_mono_type_substitute(XrTypeRef *type, XrMonoTypeMap *type_map,
                                           int type_map_count);

/* ========== Mono Instance Tracking ========== */

typedef struct {
    const char *generic_name;  // Original generic name
    XrTypeRef **type_args;     // Concrete type ref arguments
    int type_arg_count;
    const char *mangled_name;  // Mangled name (heap-allocated)
    bool is_class_generic;     // true for class/struct generics
    XaMonoThrowEffect throw_effect;
    /* Expansion provenance. `parent` is the index of the instance whose
     * specialized body requested this one, or -1 for a site in user-written
     * code; `depth` is that chain's length. Together they reconstruct the
     * instantiation chain printed by the E0388 diagnostic -- without it a
     * depth error names a type the user never wrote. */
    int parent;
    int depth;
} XaMonoInstance;

typedef struct {
    XaMonoInstance *instances;
    int count;
    int capacity;
    XaAnalyzer *analyzer;  // borrowed; enables call-site HOF effect specialization
    /* Index of the instance whose clone is currently being scanned for nested
     * instantiations, or -1 while scanning user-written code. */
    int expanding;
    /* A budget diagnostic is reported once. The pass keeps running so the user
     * still gets the rest of the program's errors, but every later
     * instantiation would report the same exhausted budget. */
    bool budget_reported;
} XaMonoCollector;

XR_FUNC void xa_mono_collector_init(XaMonoCollector *c);
XR_FUNC void xa_mono_collector_free(XaMonoCollector *c);

// Add a generic instantiation. Returns the mangled name (owned by collector),
// or NULL when a budget is exhausted -- in which case a diagnostic has been
// reported and compilation will fail.
// Concrete type arguments define identity for function, class, and struct instances.
XR_FUNC const char *xa_mono_collector_add(XaMonoCollector *c, const char *generic_name,
                                          XrTypeRef **type_args, int type_arg_count,
                                          bool is_class_generic, const XrLocation *loc);

/* ========== Mono Pass ========== */

/* Run the full monomorphization pass on a program AST: collect generic
 * declarations and instantiation sites, clone+substitute for each concrete
 * type combination, inject into the program, rewrite call sites. A program
 * with no generics is a no-op.
 *
 * `external_roots` are dependency-module ASTs (may be NULL/0 for a single
 * unit). The current module may instantiate generic value structs imported
 * from them, and may rewrite imported generic class/function namespace calls
 * to specializations injected by their defining modules. Value-struct clones
 * stay local to the using module so lowering has a concrete local layout.
 *
 * `analyzer` is required, not optional: it is both the throw-effect oracle
 * that splits generic HOFs into MAY_THROW / NO_THROW bodies and the sink for
 * E0387/E0388. A pass that cannot report an exhausted budget could only
 * respond by silently leaving calls generic.
 *
 * Returns false when a budget diagnostic was reported; the caller must not
 * proceed to lowering. */
XR_FUNC bool xa_mono_pass(AstNode *root, AstNode **external_roots, int external_root_count,
                          XrVMRuntime *isolate, XaAnalyzer *analyzer);

#endif  // XANALYZER_MONO_H
