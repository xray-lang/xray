/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_graph.h - Module dependency graph
 *
 * KEY CONCEPT:
 *   Holds the complete set of modules reachable from an entry file,
 *   built via BFS using XrModuleResolver.  Provides a valid topological
 *   order (or detects cycles) for downstream passes.
 */

#ifndef XMODULE_GRAPH_H
#define XMODULE_GRAPH_H

#include "../base/xdefs.h"
#include "../base/xhashmap.h"
#include "../base/xstable_id.h"
#include "xmodule_resolver.h"
#include "xmodule_fingerprint.h"

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct AstNode;
struct XrModule;
struct XrCompilerSession;
struct XrVMRuntime;

/* ========== Module Spec Status ========== */

typedef enum {
    XR_MODSPEC_PENDING = 0, /* Discovered but not yet parsed */
    XR_MODSPEC_PARSED,      /* Source parsed; AST available */
    XR_MODSPEC_RESOLVED,    /* All imports resolved via resolver */
    XR_MODSPEC_ANALYZED,    /* Analyzed: typed info available */
    XR_MODSPEC_LOWERED,     /* IR lowered; ready for codegen */
} XrModSpecStatus;

/* ========== Module Spec ========== */

/* One module in the graph. Owns its AST, durable identity, and
 * the list of edges (import dependencies). */
typedef struct XrModuleSpec {
    char *canonical;      /* Canonical module ID (owned, xr_compile_resources_free) */
    char *logical_path;   /* Authority-root-relative path (owned, xr_compile_resources_free) */
    char *source_path;    /* Absolute path to source file (owned) */
    XrModuleKind kind;    /* stdlib / file / package */
    XrModuleRepresentation representation;
    const XrModuleResourceBinding *resource; /* Catalog outlives this graph. */
    XrModuleIdentityAuthority authority; /* Owned typed authority */
    bool embedded_source; /* source_path is a diagnostic-only embedded stdlib path */
    XrFingerprint source_content_fingerprint; /* Full source bytes; never a locator */
    XrModSpecStatus status;

    struct AstNode *ast; /* Parsed AST (owned; freed via xr_program_destroy) */

    /* Dependency edges: indices into XrModuleGraph.specs[] */
    int *dep_indices; /* Array of spec indices this module imports from */
    int dep_count;
    int dep_capacity;

    /* Exported semantic symbols: name -> XaSymbol* (populated by analyzer).
     * The symbol pointers are borrowed from analyzer-owned scopes; their links
     * carry type, class, enum, ADT payload, and generic metadata. Only valid
     * while the analyzer that filled the graph is alive. If export collection
     * observes compiler-only recovery metadata, the whole export table is
     * invalid rather than partially populated. */
    XrHashMap *export_symbols;
    bool export_symbols_invalid;

    /* Topological sort metadata */
    int topo_index; /* Position in topo_order (-1 if not yet assigned) */
    int scc_id;     /* SCC id (size>1 means cycle) */
} XrModuleSpec;

/* ========== Module Graph ========== */

typedef struct XrModuleGraph {
    XrCompileResources *resources; /* Shared with the borrowed session and resolver. */
    XrModuleSpec *specs; /* Dynamic array of module specs */
    int spec_count;
    int spec_capacity;

    /* Canonical → spec index lookup (O(1) by hash) */
    XrHashMap *id_index;

    /* Topological order: spec indices in valid init order (leaves first) */
    int *topo_order;
    int topo_count;

    /* Cycle detection results */
    bool has_cycle;
    char *cycle_desc; /* Human-readable cycle description (owned, or NULL) */

    /* First import that did not resolve, if any (owned, or NULL). The walk
     * cannot report from where it happens -- it is void and several frames
     * below the caller holding out_err -- so it records the first failure and
     * the build entry point turns it into the build's error. Dropping it was
     * how `import "./typo"` and a package missing from xray.toml compiled
     * clean and only surfaced, if at all, at run time. */
    char *unresolved_error;

    /* The resolver used during build */
    XrModuleResolver *resolver;

    /* Compiler session used for parsing graph sources. */
    struct XrCompilerSession *compiler_session;

    /* Optional VM host association; compiler-only graphs leave this NULL. */
    struct XrVMRuntime *X;

    /* Entry module index */
    int entry_index;
    /* Failure remains observable even when allocating its diagnostic fails. */
    XrModuleStatus resolution_status;
} XrModuleGraph;

/* ========== API ========== */

/* Create a new empty module graph.  The resolver is borrowed (not freed).
 * Session, resolver and its catalog remain alive until graph destruction.
 * All three owners must share resources; failure preserves output. */
XR_FUNC XrModuleStatus xr_compile_module_graph_new(XrCompileResources *resources,
    struct XrCompilerSession *compiler_session, XrModuleResolver *resolver, XrModuleGraph **output);

/* Free the graph and all owned specs/ASTs. */
XR_FUNC void xr_compile_module_graph_free(XrModuleGraph *g);

/* Build an empty graph by BFS from an entry source file.
 * Parses each discovered module and collects its import edges.
 * Returns XR_MODULE_OK on success, otherwise the precise failure status.
 * An optional diagnostic uses xr_compile_resources_free; its allocation
 * cannot replace the first typed cause. Failure can leave partial discovery
 * owned by the graph, which the caller must discard. */
XR_FUNC XrModuleStatus xr_compile_module_graph_build(XrModuleGraph *g, const char *entry_path,
                                  const XrModuleIdentityAuthority *entry_authority,
                                  char **out_err);

/* Add a source and its imports without changing the existing entry or adding
 * a synthetic import edge. Existing identities must retain their exact source
 * authority. New modules invalidate the topological order. This extends the
 * checking graph, not an execution or initialization closure. On error discard
 * the graph; partial discovery is owned by it and freed with it. */
XR_FUNC XrModuleStatus xr_compile_module_graph_include(XrModuleGraph *g, const char *source_path,
                                    const XrModuleIdentityAuthority *authority, char **out_err);

/* Build the graph from an in-memory entry source.
 * The caller-supplied memory authority is mandatory.
 * Relative imports fail because memory modules have no physical root. */
XR_FUNC XrModuleStatus xr_compile_module_graph_build_source(XrModuleGraph *g,
                                         const XrModuleIdentityAuthority *entry_authority,
                                         const char *entry_source, char **out_err);

/* Check the caller's exact bytes under a typed logical source identity.
 * A physical root requires a matching rooted locator. A rootless stdlib source
 * may use a diagnostic locator but cannot resolve relative file imports. */
XR_FUNC XrModuleStatus xr_compile_module_graph_build_logical_source(XrModuleGraph *g,
    const XrModuleIdentityAuthority *authority, const char *logical_path,
    const char *source_path, const char *source, char **out_err);

/* Include exact caller-owned logical source under the same typed authority
 * rules as build_logical_source. The original entry is preserved. Reusing a
 * canonical identity requires the same authority, locator and content digest.
 * Failure leaves all partial discovery owned; discard the failed graph. */
XR_FUNC XrModuleStatus xr_compile_module_graph_include_logical_source(XrModuleGraph *g,
    const XrModuleIdentityAuthority *authority, const char *logical_path,
    const char *source_path, const char *source, char **out_err);

/* Add an ordinary dependency edge between admitted graph indices. Duplicate
 * edges are idempotent; a fresh edge invalidates the prior topological order.
 * Cycles remain subject to the existing topological-sort rejection. */
XR_FUNC XrModuleStatus xr_compile_module_graph_add_dependency(XrModuleGraph *g, int from, int to);

/* Run topological sort (Tarjan SCC).
 * After success, g->topo_order is filled and g->has_cycle indicates cycles.
 * Returns XR_MODULE_OK without cycles, XR_MODULE_INVALID for a cycle, or
 * a precise allocation/work failure. Resource failure preserves any prior
 * complete order and node metadata. */
XR_FUNC XrModuleStatus xr_compile_module_graph_topological_sort(XrModuleGraph *g);

/* The name this module is imported under at run time.
 *
 * A dependency that is preloaded, or bundled without its bytecode, is
 * reconstituted by handing this string to xr_module_import, whose contract is
 * an import specifier: a bare name for the standard library, a path for a
 * source file. The canonical id is neither -- it is the length-framed durable
 * identity the resolver builds for graph keys, and it contains '/', so feeding
 * it back in is read as a third-party package and fails closed on lookup.
 *
 * Both the preload path and the bundle writer must ask this, or one of them
 * ends up storing an identity where the other stores a name. */
XR_FUNC const char *xr_module_spec_import_name(const XrModuleSpec *spec);

/* Typed lookups publish an index (-1 when absent) only on OK. */
XR_FUNC XrModuleStatus xr_compile_module_graph_find(const XrModuleGraph *g, const char *canonical, int *output);
/* Physical-source lookup is local plumbing only; it is never a graph identity key. */
XR_FUNC XrModuleStatus xr_compile_module_graph_find_source(const XrModuleGraph *g, const char *source_path, int *output);
/* Resolve a named stdlib/package coordinate only through one exact importer
 * edge already admitted by the graph resolver. Returns a spec index or -1. */
XR_FUNC XrModuleStatus xr_compile_module_graph_find_named_dependency(const XrModuleGraph *g, const char *importer_path,
    const char *specifier, int *output);

/* True only when `decl` is one exact top-level declaration owned by `spec`.
 * Compiler-private
 * bindings use pointer identity here; names are not authority. */
XR_FUNC XrModuleStatus xr_compile_module_graph_owns_top_level_decl(const XrModuleGraph *graph,
    const XrModuleSpec *spec, const struct AstNode *decl, bool *output);

#endif  // XMODULE_GRAPH_H
