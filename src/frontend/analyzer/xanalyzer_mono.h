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
#include "xa_node_table.h"

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
 *                         A depth counter cannot tell a diverging chain from a
 *                         deep but finite one, so the limit must clear any
 *                         plausible real chain by a wide margin -- rejecting
 *                         honest code to catch divergence sooner is a bad
 *                         trade when divergence is caught either way.
 *
 *   XR_MONO_MAX_INSTANCES bounds *breadth*. Each instance clones a whole
 *                         declaration, so this is a compile-time memory
 *                         backstop, not a language rule. It is set far above
 *                         any plausible real program: a program that trips it
 *                         has a generic expansion problem worth seeing.
 *
 * Both limits fail with E0388 or E0389.
 * Exhaustion must never leave a generic call in place.
 *
 * That would violate the no-box verifier contract.
 */
#define XR_MONO_MAX_DEPTH 128
#define XR_MONO_MAX_INSTANCES 16384

typedef struct XaMonoBudget {
    uint32_t max_depth;
    uint32_t max_instances;
} XaMonoBudget;

/* One usage object spans the source build's declaring modules.
 * It accumulates usage; it is not a
 * one-pass snapshot. */
typedef struct XaMonoUsage {
    uint32_t instance_count;
    uint32_t max_depth;
} XaMonoUsage;

XR_FUNC XaMonoBudget xa_mono_default_budget(void);
XR_FUNC bool xa_mono_budget_valid(const XaMonoBudget *budget);

typedef struct XaAnalyzer XaAnalyzer;

/* ========== Name Mangling ========== */

// Generate mangled name for a monomorphized function/class.
// Result is heap-allocated; caller must free.
// Example: mangle("identity", [int_tref], 1) -> "identity$i64"
XR_FUNC char *xr_mono_mangle(const char *name, XrTypeRef **type_args, int count);

// Analyzer-aware specialization name. Declaration-backed nominal arguments
// append stable semantic identity, so equal spellings from different modules
// cannot share a specialization.
XR_FUNC char *xr_mono_mangle_in_analyzer(XaAnalyzer *analyzer, const char *name,
                                         XrTypeRef **type_args, int count);

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
    struct XrType *concrete_semantic_type;  // Exact analyzer type, when available
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
XR_FUNC XrTypeRef *xr_mono_type_substitute_in_analyzer(XaAnalyzer *analyzer, XrTypeRef *type,
                                                       XrMonoTypeMap *type_map, int type_map_count);

/* ========== Mono Instance Tracking ========== */

typedef struct {
    const char *generic_name;              // owned diagnostic/display name
    XaGenericSpecializationFact identity;  // owns both pointer arrays
    const char *mangled_name;              // owned private executable name
    AstNode *materialized_decl;            // borrowed arena node for this exact instance
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
    AstNode *rewrite_root;  // borrowed while rewriting compiler-owned import bindings
    /* Number of declaration type annotations rewritten from a generic instance
     * (Box<int>) to its mangled name (Box$i64). The rewrite walk snapshots this
     * around each class/struct body so a declaration whose member signatures
     * changed can be marked for re-collection in the post-mono analysis pass. */
    uint32_t tref_rewrite_count;
    /* Index of the instance whose clone is currently being scanned for nested
     *
     * instantiations, or -1 while scanning user-written code. */
    int expanding;
    uint32_t max_depth;
    uint32_t max_instances;
    uint32_t instance_offset;
    uint32_t max_observed_depth;
    /* Generic template bodies are scanned once to attach exact declaration
     * facts for later clone substitution, without treating open-template
     * operations as executable instantiation roots. */
    bool record_only;
    /* A budget diagnostic is reported once. The pass keeps running so the user
     * still gets the rest of the program's errors, but every later
     * instantiation would report the same exhausted budget. */
    bool budget_reported;
    bool rewrite_failed;
} XaMonoCollector;

XR_FUNC void xa_mono_collector_init(XaMonoCollector *c);
XR_FUNC void xa_mono_collector_free(XaMonoCollector *c);

// Add one exact generic specialization. The identity carries independent
// receiver and declaration tuples plus its effect dimension. Returns the
// private executable name owned by the collector, or NULL after reporting an
// invalid identity or exhausted budget.
XR_FUNC const char *xa_mono_collector_add(XaMonoCollector *c, const char *generic_name,
                                          const XaGenericSpecializationFact *identity,
                                          const XrLocation *loc);

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
 * `analyzer` is required. It classifies generic HOF throw effects.
 * It also receives E0388 and
 * E0389 budget diagnostics.
 * Without diagnostics, exhaustion could leave calls generic.
 *
 *
 * `usage` accumulates across a complete source build.
 * Thus max_instances is a whole-program
 * budget.
 * Declaring modules can still be specialized in separate passes.
 * Returns false when a
 * budget diagnostic was reported; the caller must not proceed to lowering. */
XR_FUNC bool xa_mono_pass(AstNode *root, AstNode **external_roots, int external_root_count,
                          XrVMRuntime *isolate, const XaMonoBudget *budget, XaMonoUsage *usage,
                          XaAnalyzer *analyzer);

#endif  // XANALYZER_MONO_H
