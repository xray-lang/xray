/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_module_link.c - SCC-based module linker
 *
 * Resolves cross-module imports by matching XiModuleImport entries to
 * XiModuleExport entries across a module graph.  Uses Tarjan's algorithm
 * to assign SCC IDs for cycle detection and initialization ordering.
 *
 * After linking:
 *   - Each import's `resolved` pointer is set to the matching export.
 *   - Each module's `scc_id` and `link_status` are assigned.
 *   - Unresolved imports return a nonzero count for error reporting.
 */

#include "xi.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#include <string.h>
#include <stdio.h>

/* Maximum modules supported in a single link operation. */
#define LINK_MAX_MODULES 256

/* ========== Import Resolution ========== */

/* Find the export matching an import by module path and member name.
 * Searches all modules for a matching path and export name.
 * Returns the matching XiModuleExport* or NULL. */
static XiModuleExport *find_export(XiModule **modules, int nmodules,
                                    const XiModuleImport *imp) {
    XR_DCHECK(imp != NULL, "find_export: NULL import");
    if (!imp->module_path) return NULL;

    for (int m = 0; m < nmodules; m++) {
        XiModule *mod = modules[m];
        if (!mod || !mod->path) continue;

        /* Match by path suffix (the import path is relative, e.g. "./math") */
        const char *mod_base = strrchr(mod->path, '/');
        const char *imp_base = strrchr(imp->module_path, '/');
        const char *mod_name = mod_base ? mod_base + 1 : mod->path;
        const char *imp_name = imp_base ? imp_base + 1 : imp->module_path;

        /* Strip .xr extension for comparison */
        size_t mod_len = strlen(mod_name);
        if (mod_len > 3 && strcmp(mod_name + mod_len - 3, ".xr") == 0)
            mod_len -= 3;

        if (strncmp(mod_name, imp_name, mod_len) != 0) continue;
        if (imp_name[mod_len] != '\0' && imp_name[mod_len] != '.') continue;

        /* Found the exporting module — search for the named export */
        if (!imp->member_name) {
            /* Whole-module import (namespace) — no specific export to resolve */
            return NULL;
        }
        for (uint16_t ei = 0; ei < mod->nexports; ei++) {
            if (mod->exports[ei].name &&
                strcmp(mod->exports[ei].name, imp->member_name) == 0) {
                return &mod->exports[ei];
            }
        }
    }
    return NULL;
}

/* ========== Tarjan SCC ========== */

typedef struct {
    int index;          /* Tarjan discovery index */
    int lowlink;        /* lowest reachable index */
    bool on_stack;      /* currently on the Tarjan stack */
} TarjanNode;

typedef struct {
    XiModule **modules;
    int nmodules;
    TarjanNode *nodes;
    int *stack;
    int stack_top;
    int next_index;
    int next_scc;
} TarjanCtx;

/* Find module index by pointer. */
static int find_module_index(XiModule **modules, int nmodules, XiModule *target) {
    for (int i = 0; i < nmodules; i++) {
        if (modules[i] == target) return i;
    }
    return -1;
}

/* Find which module a given path resolves to. */
static int find_module_by_path(XiModule **modules, int nmodules,
                                const char *import_path) {
    if (!import_path) return -1;
    const char *imp_base = strrchr(import_path, '/');
    const char *imp_name = imp_base ? imp_base + 1 : import_path;

    for (int m = 0; m < nmodules; m++) {
        if (!modules[m] || !modules[m]->path) continue;
        const char *mod_base = strrchr(modules[m]->path, '/');
        const char *mod_name = mod_base ? mod_base + 1 : modules[m]->path;
        size_t mod_len = strlen(mod_name);
        if (mod_len > 3 && strcmp(mod_name + mod_len - 3, ".xr") == 0)
            mod_len -= 3;
        if (strncmp(mod_name, imp_name, mod_len) == 0 &&
            (imp_name[mod_len] == '\0' || imp_name[mod_len] == '.'))
            return m;
    }
    return -1;
}

static void tarjan_strongconnect(TarjanCtx *tc, int v) {
    XR_DCHECK(v >= 0 && v < tc->nmodules, "tarjan: bad index");
    TarjanNode *vn = &tc->nodes[v];
    vn->index = tc->next_index;
    vn->lowlink = tc->next_index;
    tc->next_index++;
    tc->stack[tc->stack_top++] = v;
    vn->on_stack = true;

    /* Visit all modules that this module imports from */
    XiModule *mod = tc->modules[v];
    if (mod) {
        for (uint16_t i = 0; i < mod->nimports; i++) {
            int w = find_module_by_path(tc->modules, tc->nmodules,
                                         mod->imports[i].module_path);
            if (w < 0) continue;

            if (tc->nodes[w].index < 0) {
                tarjan_strongconnect(tc, w);
                if (tc->nodes[w].lowlink < vn->lowlink)
                    vn->lowlink = tc->nodes[w].lowlink;
            } else if (tc->nodes[w].on_stack) {
                if (tc->nodes[w].index < vn->lowlink)
                    vn->lowlink = tc->nodes[w].index;
            }
        }
    }

    /* Root of an SCC — pop all members */
    if (vn->lowlink == vn->index) {
        int scc_id = tc->next_scc++;
        int w;
        do {
            XR_DCHECK(tc->stack_top > 0, "tarjan: stack underflow");
            w = tc->stack[--tc->stack_top];
            tc->nodes[w].on_stack = false;
            if (tc->modules[w])
                tc->modules[w]->scc_id = (int16_t)scc_id;
        } while (w != v);
    }
}

/* ========== Public API ========== */

XR_FUNC int xi_module_link_resolve(XiModule **modules, int nmodules) {
    XR_DCHECK(modules != NULL || nmodules == 0,
              "xi_module_link_resolve: NULL modules with nonzero count");
    if (nmodules <= 0) return 0;
    if (nmodules > LINK_MAX_MODULES) {
        fprintf(stderr, "[xi_module_link] too many modules (%d > %d)\n",
                nmodules, LINK_MAX_MODULES);
        return -1;
    }

    int unresolved = 0;

    /* Resolve all imports */
    for (int m = 0; m < nmodules; m++) {
        XiModule *mod = modules[m];
        if (!mod) continue;
        mod->link_status = XI_LINK_IN_PROGRESS;

        for (uint16_t i = 0; i < mod->nimports; i++) {
            XiModuleImport *imp = &mod->imports[i];
            imp->resolved = find_export(modules, nmodules, imp);
            if (!imp->resolved && imp->member_name) {
                /* Whole-module (namespace) imports don't need resolution */
                unresolved++;
                fprintf(stderr, "[xi_module_link] unresolved: %s.%s from %s\n",
                        imp->module_path ? imp->module_path : "?",
                        imp->member_name ? imp->member_name : "*",
                        mod->name ? mod->name : "?");
            }
        }
    }

    /* Tarjan SCC assignment */
    TarjanNode *nodes = (TarjanNode *)xr_calloc(nmodules, sizeof(TarjanNode));
    int *stack = (int *)xr_calloc(nmodules, sizeof(int));
    if (!nodes || !stack) {
        xr_free(nodes);
        xr_free(stack);
        return unresolved;
    }

    for (int i = 0; i < nmodules; i++) {
        nodes[i].index = -1;
        nodes[i].lowlink = -1;
        nodes[i].on_stack = false;
    }

    TarjanCtx tc = {
        .modules = modules,
        .nmodules = nmodules,
        .nodes = nodes,
        .stack = stack,
        .stack_top = 0,
        .next_index = 0,
        .next_scc = 0,
    };

    for (int i = 0; i < nmodules; i++) {
        if (nodes[i].index < 0)
            tarjan_strongconnect(&tc, i);
    }

    xr_free(nodes);
    xr_free(stack);

    /* Mark link status */
    for (int m = 0; m < nmodules; m++) {
        if (modules[m])
            modules[m]->link_status = XI_LINK_LINKED;
    }

    return unresolved;
}
