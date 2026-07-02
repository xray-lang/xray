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
#include "xmodule_resolver.h"

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct AstNode;
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

/* One module in the graph.  Owns its AST, canonical ID, and
 * the list of edges (import dependencies). */
typedef struct XrModuleSpec {
    char *canonical;   /* Canonical module ID (owned, xr_free) */
    char *source_path; /* Absolute path to source file (owned) */
    XrModuleKind kind; /* stdlib / file / package */
    XrModSpecStatus status;

    struct AstNode *ast; /* Parsed AST (owned; freed via xr_program_destroy) */

    /* Dependency edges: indices into XrModuleGraph.specs[] */
    int *dep_indices; /* Array of spec indices this module imports from */
    int dep_count;
    int dep_capacity;

    /* Exported semantic symbols: name -> XaSymbol* (populated by analyzer).
     * The symbol pointers are borrowed from analyzer-owned scopes; their links
     * carry type, class, enum, ADT payload, and generic metadata. Only valid
     * while the analyzer that filled the graph is alive. */
    XrHashMap *export_symbols;

    /* Topological sort metadata */
    int topo_index; /* Position in topo_order (-1 if not yet assigned) */
    int scc_id;     /* SCC id (size>1 means cycle) */
} XrModuleSpec;

/* ========== Module Graph ========== */

typedef struct XrModuleGraph {
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

    /* The resolver used during build */
    XrModuleResolver *resolver;

    /* Compiler session used for parsing graph sources. */
    struct XrCompilerSession *compiler_session;

    /* VM host for AST factories while parser/analyzer migration is in progress. */
    struct XrVMRuntime *X;

    /* Entry module index */
    int entry_index;
} XrModuleGraph;

/* ========== API ========== */

/* Create a new empty module graph.  The resolver is borrowed (not freed).
 * The compiler session is borrowed and used for parsing source files. */
XR_FUNC XrModuleGraph *xr_module_graph_new(struct XrCompilerSession *compiler_session,
                                           XrModuleResolver *resolver);

/* Free the graph and all owned specs/ASTs. */
XR_FUNC void xr_module_graph_free(XrModuleGraph *g);

/* Build the graph by BFS from an entry source file.
 * Parses each discovered module and collects its import edges.
 * Returns 0 on success, -1 on error (e.g. file not found).
 * On error, *out_err is set to a descriptive message (caller frees). */
XR_FUNC int xr_module_graph_build(XrModuleGraph *g, const char *entry_path, char **out_err);

/* Run topological sort (Tarjan SCC).
 * After success, g->topo_order is filled and g->has_cycle indicates cycles.
 * Returns 0 on success (no cycle), -1 if cycles detected (g->cycle_desc set). */
XR_FUNC int xr_module_graph_topological_sort(XrModuleGraph *g);

/* Lookup a module spec by canonical ID.  Returns index or -1. */
XR_FUNC int xr_module_graph_find(const XrModuleGraph *g, const char *canonical);

#endif  // XMODULE_GRAPH_H
