/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_graph.c - Module dependency graph: BFS build + topological sort
 *
 * KEY CONCEPT:
 *   Starting from an entry file, discover all reachable modules by parsing
 *   each file and resolving its import specifiers.  Then run Tarjan SCC to
 *   produce a valid initialization order or detect cycles.
 */

#include "xmodule_graph.h"
#include "../base/xchecks.h"
#include "../base/xfileio.h"
#include "../base/xforward_decl.h"
#include "../base/xlog.h"
#include "../base/xmalloc.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../os/os_fs.h"

#include <stdio.h>
#include <string.h>

/* Forward declaration for AST destruction */
extern void xr_program_destroy(struct AstNode *ast);

/* ========== Internal Constants ========== */

#define GRAPH_INITIAL_CAP 16
#define GRAPH_MAX_MODULES 1024

/* ========== Lifecycle ========== */

XR_FUNC XrModuleGraph *xr_module_graph_new(XrayIsolate *X, XrModuleResolver *resolver) {
    XR_DCHECK(resolver != NULL, "xr_module_graph_new: NULL resolver");
    XrModuleGraph *g = xr_calloc(1, sizeof(XrModuleGraph));
    if (!g)
        return NULL;

    g->specs = xr_calloc(GRAPH_INITIAL_CAP, sizeof(XrModuleSpec));
    if (!g->specs) {
        xr_free(g);
        return NULL;
    }
    g->spec_capacity = GRAPH_INITIAL_CAP;
    g->id_index = xr_hashmap_new();
    g->resolver = resolver;
    g->X = X;
    g->entry_index = -1;
    return g;
}

XR_FUNC void xr_module_graph_free(XrModuleGraph *g) {
    if (!g)
        return;

    for (int i = 0; i < g->spec_count; i++) {
        XrModuleSpec *s = &g->specs[i];
        xr_free(s->canonical);
        xr_free(s->source_path);
        if (s->ast)
            xr_program_destroy(s->ast);
        xr_free(s->dep_indices);
        if (s->exports)
            xr_hashmap_free(s->exports);
    }
    xr_free(g->specs);
    xr_hashmap_free(g->id_index);
    xr_free(g->topo_order);
    xr_free(g->cycle_desc);
    xr_free(g);
}

/* ========== Internal Helpers ========== */

/* Add a new spec to the graph.  Returns the index, or -1 on OOM. */
static int graph_add_spec(XrModuleGraph *g, const char *canonical, const char *source_path,
                          XrModuleKind kind) {
    XR_DCHECK(g != NULL, "graph_add_spec: NULL graph");
    XR_DCHECK(canonical != NULL, "graph_add_spec: NULL canonical");

    if (g->spec_count >= GRAPH_MAX_MODULES) {
        xr_log_warning("module_graph", "module limit (%d) reached", GRAPH_MAX_MODULES);
        return -1;
    }

    if (g->spec_count >= g->spec_capacity) {
        int new_cap = g->spec_capacity * 2;
        XrModuleSpec *tmp = xr_realloc(g->specs, (size_t) new_cap * sizeof(XrModuleSpec));
        if (!tmp)
            return -1;
        g->specs = tmp;
        memset(&g->specs[g->spec_count], 0,
               (size_t) (new_cap - g->spec_capacity) * sizeof(XrModuleSpec));
        g->spec_capacity = new_cap;
    }

    int idx = g->spec_count++;
    XrModuleSpec *s = &g->specs[idx];
    s->canonical = xr_strdup(canonical);
    s->source_path = source_path ? xr_strdup(source_path) : NULL;
    s->kind = kind;
    s->status = XR_MODSPEC_PENDING;
    s->topo_index = -1;
    s->scc_id = -1;

    xr_hashmap_set(g->id_index, s->canonical, (void *) (intptr_t) (idx + 1));
    return idx;
}

/* Add a dependency edge from spec at `from_idx` to spec at `to_idx`. */
static void spec_add_dep(XrModuleSpec *s, int to_idx) {
    XR_DCHECK(s != NULL, "spec_add_dep: NULL spec");

    /* Deduplicate */
    for (int i = 0; i < s->dep_count; i++) {
        if (s->dep_indices[i] == to_idx)
            return;
    }

    if (s->dep_count >= s->dep_capacity) {
        int new_cap = s->dep_capacity * 2;
        if (new_cap < 4)
            new_cap = 4;
        int *tmp = xr_realloc(s->dep_indices, (size_t) new_cap * sizeof(int));
        if (!tmp)
            return;
        s->dep_indices = tmp;
        s->dep_capacity = new_cap;
    }
    s->dep_indices[s->dep_count++] = to_idx;
}

XR_FUNC int xr_module_graph_find(const XrModuleGraph *g, const char *canonical) {
    if (!g || !canonical)
        return -1;
    void *val = xr_hashmap_get(g->id_index, canonical);
    if (!val)
        return -1;
    return (int) (intptr_t) val - 1;
}

/* ========== BFS Build ========== */

/* Collect import specifiers from an AST program node and resolve each.
 * For every resolved module, ensure it exists in the graph and add an edge. */
static void collect_and_resolve_imports(XrModuleGraph *g, int spec_idx, struct AstNode *ast) {
    XR_DCHECK(ast != NULL, "collect_and_resolve_imports: NULL ast");
    XrModuleSpec *from_spec = &g->specs[spec_idx];

    /* Walk top-level statements looking for AST_IMPORT_STMT */
    if (ast->type != AST_PROGRAM)
        return;

    for (int i = 0; i < ast->as.program.count; i++) {
        struct AstNode *stmt = ast->as.program.statements[i];
        if (!stmt || stmt->type != AST_IMPORT_STMT)
            continue;

        const char *specifier = stmt->as.import_stmt.module_name;
        bool is_bare = !stmt->as.import_stmt.is_quoted;

        XrModuleId mid;
        char *err = NULL;
        int rc = xr_module_resolver_resolve(g->resolver, specifier, is_bare, from_spec->source_path,
                                            &mid, &err);
        if (rc != 0) {
            /* Resolution failed — skip (stdlib native modules won't have source) */
            xr_free(err);
            continue;
        }

        /* Stdlib native modules without source_path: no node needed in graph */
        if (mid.kind == XR_MOD_STDLIB && !mid.source_path) {
            xr_module_id_cleanup(&mid);
            continue;
        }

        /* Find or create target spec in the graph */
        int target_idx = xr_module_graph_find(g, mid.canonical);
        if (target_idx < 0) {
            target_idx = graph_add_spec(g, mid.canonical, mid.source_path, mid.kind);
        }
        xr_module_id_cleanup(&mid);

        if (target_idx >= 0) {
            /* Re-fetch from_spec pointer since realloc may have moved it */
            from_spec = &g->specs[spec_idx];
            spec_add_dep(from_spec, target_idx);
        }
    }

    from_spec = &g->specs[spec_idx];
    from_spec->status = XR_MODSPEC_RESOLVED;
}

XR_FUNC int xr_module_graph_build(XrModuleGraph *g, const char *entry_path, char **out_err) {
    XR_DCHECK(g != NULL, "xr_module_graph_build: NULL graph");
    if (!g || !entry_path) {
        if (out_err)
            *out_err = xr_strdup("NULL graph or entry_path");
        return -1;
    }

    /* Canonicalize entry path */
    char *abs_path = xr_realpath(entry_path);
    if (!abs_path) {
        if (!xr_fs_exists(entry_path)) {
            if (out_err) {
                char buf[512];
                snprintf(buf, sizeof(buf), "entry file not found: %s", entry_path);
                *out_err = xr_strdup(buf);
            }
            return -1;
        }
        abs_path = xr_strdup(entry_path);
    }

    /* Add entry spec */
    int entry_idx = graph_add_spec(g, abs_path, abs_path, XR_MOD_FILE);
    if (entry_idx < 0) {
        xr_free(abs_path);
        if (out_err)
            *out_err = xr_strdup("failed to add entry module");
        return -1;
    }
    g->entry_index = entry_idx;
    xr_free(abs_path);

    /* BFS: process each spec in queue order.
     * spec_count grows as new modules are discovered. */
    for (int qi = 0; qi < g->spec_count; qi++) {
        XrModuleSpec *spec = &g->specs[qi];

        /* Skip stdlib native (no source to parse) */
        if (!spec->source_path)
            continue;

        /* Skip already processed */
        if (spec->status >= XR_MODSPEC_RESOLVED)
            continue;

        /* Parse source */
        char *source = xr_file_read_all(spec->source_path, "r", NULL);
        if (!source) {
            xr_log_warning("module_graph", "cannot read: %s", spec->source_path);
            continue;
        }

        struct AstNode *ast = xr_parse_with_source(g->X, source, spec->source_path);
        xr_free(source);

        if (!ast) {
            xr_log_warning("module_graph", "parse failed: %s", spec->source_path);
            continue;
        }

        spec->ast = ast;
        spec->status = XR_MODSPEC_PARSED;

        /* Resolve imports and discover new modules */
        collect_and_resolve_imports(g, qi, ast);
    }

    return 0;
}

/* ========== Topological Sort (Tarjan SCC) ========== */

typedef struct {
    int index;
    int lowlink;
    bool on_stack;
} GraphTarjanNode;

typedef struct {
    XrModuleGraph *graph;
    GraphTarjanNode *nodes;
    int *stack;
    int stack_top;
    int next_index;
    int next_scc;
    int *scc_sizes; /* Track SCC sizes for cycle detection */
    int scc_cap;
} GraphTarjanCtx;

static void graph_tarjan_strongconnect(GraphTarjanCtx *tc, int v) {
    XR_DCHECK(v >= 0 && v < tc->graph->spec_count, "tarjan: bad index");
    GraphTarjanNode *vn = &tc->nodes[v];
    vn->index = tc->next_index;
    vn->lowlink = tc->next_index;
    tc->next_index++;
    tc->stack[tc->stack_top++] = v;
    vn->on_stack = true;

    /* Visit dependencies */
    XrModuleSpec *spec = &tc->graph->specs[v];
    for (int i = 0; i < spec->dep_count; i++) {
        int w = spec->dep_indices[i];
        XR_DCHECK(w >= 0 && w < tc->graph->spec_count, "tarjan: bad dep index");
        if (tc->nodes[w].index < 0) {
            graph_tarjan_strongconnect(tc, w);
            if (tc->nodes[w].lowlink < vn->lowlink)
                vn->lowlink = tc->nodes[w].lowlink;
        } else if (tc->nodes[w].on_stack) {
            if (tc->nodes[w].index < vn->lowlink)
                vn->lowlink = tc->nodes[w].index;
        }
    }

    /* Root of an SCC */
    if (vn->lowlink == vn->index) {
        int scc_id = tc->next_scc++;

        /* Grow scc_sizes if needed */
        if (scc_id >= tc->scc_cap) {
            int new_cap = tc->scc_cap * 2;
            if (new_cap < 16)
                new_cap = 16;
            int *tmp = xr_realloc(tc->scc_sizes, (size_t) new_cap * sizeof(int));
            if (tmp) {
                memset(tmp + tc->scc_cap, 0, (size_t) (new_cap - tc->scc_cap) * sizeof(int));
                tc->scc_sizes = tmp;
                tc->scc_cap = new_cap;
            }
        }

        int scc_size = 0;
        int w;
        do {
            XR_DCHECK(tc->stack_top > 0, "tarjan: stack underflow");
            w = tc->stack[--tc->stack_top];
            tc->nodes[w].on_stack = false;
            tc->graph->specs[w].scc_id = scc_id;
            scc_size++;
        } while (w != v);

        if (scc_id < tc->scc_cap)
            tc->scc_sizes[scc_id] = scc_size;
    }
}

/* Build a human-readable cycle description from the first SCC with size > 1. */
static char *build_cycle_desc(XrModuleGraph *g, int *scc_sizes, int nscc) {
    for (int s = 0; s < nscc; s++) {
        if (scc_sizes[s] <= 1)
            continue;

        /* Collect module names in this SCC */
        char buf[1024];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - (size_t) pos, "circular dependency: ");
        bool first = true;
        for (int i = 0; i < g->spec_count && pos < (int) sizeof(buf) - 64; i++) {
            if (g->specs[i].scc_id == s) {
                if (!first)
                    pos += snprintf(buf + pos, sizeof(buf) - (size_t) pos, " -> ");
                const char *name = g->specs[i].canonical;
                /* Use basename for readability */
                const char *slash = strrchr(name, '/');
                const char *display = slash ? slash + 1 : name;
                pos += snprintf(buf + pos, sizeof(buf) - (size_t) pos, "%s", display);
                first = false;
            }
        }
        return xr_strdup(buf);
    }
    return NULL;
}

XR_FUNC int xr_module_graph_topological_sort(XrModuleGraph *g) {
    XR_DCHECK(g != NULL, "xr_module_graph_topological_sort: NULL graph");
    if (!g || g->spec_count == 0)
        return 0;

    int n = g->spec_count;

    GraphTarjanNode *nodes = xr_calloc(n, sizeof(GraphTarjanNode));
    int *stack = xr_calloc(n, sizeof(int));
    if (!nodes || !stack) {
        xr_free(nodes);
        xr_free(stack);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        nodes[i].index = -1;
        nodes[i].lowlink = -1;
        nodes[i].on_stack = false;
    }

    GraphTarjanCtx tc = {
        .graph = g,
        .nodes = nodes,
        .stack = stack,
        .stack_top = 0,
        .next_index = 0,
        .next_scc = 0,
        .scc_sizes = NULL,
        .scc_cap = 0,
    };

    for (int i = 0; i < n; i++) {
        if (nodes[i].index < 0)
            graph_tarjan_strongconnect(&tc, i);
    }

    /* Check for cycles: SCC with size > 1, or size == 1 with self-edge */
    g->has_cycle = false;
    for (int s = 0; s < tc.next_scc; s++) {
        if (s < tc.scc_cap && tc.scc_sizes[s] > 1) {
            g->has_cycle = true;
            break;
        }
    }
    /* Also check for self-loops (SCC size==1 but node depends on itself) */
    if (!g->has_cycle) {
        for (int i = 0; i < n; i++) {
            XrModuleSpec *spec = &g->specs[i];
            for (int d = 0; d < spec->dep_count; d++) {
                if (spec->dep_indices[d] == i) {
                    g->has_cycle = true;
                    break;
                }
            }
            if (g->has_cycle)
                break;
        }
    }

    if (g->has_cycle) {
        xr_free(g->cycle_desc);
        g->cycle_desc = build_cycle_desc(g, tc.scc_sizes, tc.next_scc);
    }

    /* Build topological order from SCC ids.
     * Tarjan produces SCCs in reverse topological order. */
    xr_free(g->topo_order);
    g->topo_order = xr_calloc(n, sizeof(int));
    g->topo_count = n;

    if (g->topo_order) {
        /* Tarjan emits SCCs in reverse topological order:
         * SCC 0 is the last in dependency chain (leaf), higher IDs are earlier.
         * For init order (leaves first), iterate SCC 0..next_scc-1. */
        int pos = 0;
        for (int scc = 0; scc < tc.next_scc && pos < n; scc++) {
            for (int i = 0; i < n; i++) {
                if (g->specs[i].scc_id == scc) {
                    g->topo_order[pos] = i;
                    g->specs[i].topo_index = pos;
                    pos++;
                }
            }
        }
        g->topo_count = pos;
    }

    xr_free(nodes);
    xr_free(stack);
    xr_free(tc.scc_sizes);

    return g->has_cycle ? -1 : 0;
}
