/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xresolver.c - Dependency tree resolver implementation
 *
 * KEY CONCEPT:
 *   Resolves package dependencies into a directed acyclic graph (DAG).
 *   Performs cycle detection, topological sorting, and version selection.
 */

#include "xresolver.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16

static XrDepNode* node_new(const char *name, const char *constraint_str) {
    XR_DCHECK(name != NULL, "node_new: NULL name");
    XrDepNode *node = (XrDepNode*)xr_malloc(sizeof(XrDepNode));
    if (!node) return NULL;
    memset(node, 0, sizeof(XrDepNode));
    
    node->name = xr_strdup(name);
    node->constraint_str = xr_strdup(constraint_str);
    
    if (constraint_str) {
        xr_constraint_parse(constraint_str, &node->constraint);
    }
    
    node->dep_capacity = 8;
    node->dependencies = (XrDepNode**)xr_malloc(node->dep_capacity * sizeof(XrDepNode*));
    if (node->dependencies) {
        memset(node->dependencies, 0, node->dep_capacity * sizeof(XrDepNode*));
    }
    
    return node;
}

/*
 * Free dependency node.
 */
static void node_free(XrDepNode *node) {
    if (!node) return;
    
    xr_free(node->name);
    xr_free(node->constraint_str);
    xr_constraint_free(&node->constraint);
    xr_semver_free(&node->resolved_version);
    xr_free(node->dependencies);
    xr_free(node);
}

/*
 * Add dependency to node.
 */
static bool node_add_dep(XrDepNode *node, XrDepNode *dep) {
    if (!node || !dep) return false;
    
    // Check if already exists
    for (int i = 0; i < node->dep_count; i++) {
        if (strcmp(node->dependencies[i]->name, dep->name) == 0) {
            return true;  // Already exists
        }
    }
    
    // Check capacity
    if (node->dep_count >= node->dep_capacity) {
        int old_cap = node->dep_capacity;
        int new_cap = old_cap * 2;
        XrDepNode **new_deps = (XrDepNode**)xr_realloc(node->dependencies, new_cap * sizeof(XrDepNode*));
        if (!new_deps) return false;
        node->dependencies = new_deps;
        node->dep_capacity = new_cap;
    }
    
    node->dependencies[node->dep_count++] = dep;
    XR_DCHECK(node->dep_count <= node->dep_capacity, "node_add_dep: count > capacity");
    return true;
}

/* ========== Package Info API ========== */

void xr_package_info_free(XrPackageInfo *info) {
    if (!info) return;
    
    xr_free(info->name);
    
    for (int i = 0; i < info->version_count; i++) {
        xr_free(info->versions[i]);
    }
    xr_free(info->versions);
    
    for (int i = 0; i < info->dep_count; i++) {
        xr_free(info->deps[i]);
    }
    xr_free(info->deps);
    
    xr_free(info);
}

/* ========== Dependency Graph API Implementation ========== */

XrDepGraph* xr_depgraph_new(void) {
    XrDepGraph *graph = (XrDepGraph*)xr_malloc(sizeof(XrDepGraph));
    if (!graph) return NULL;
    memset(graph, 0, sizeof(XrDepGraph));
    
    graph->node_capacity = INITIAL_CAPACITY;
    graph->nodes = (XrDepNode**)xr_malloc(graph->node_capacity * sizeof(XrDepNode*));
    
    graph->root_capacity = INITIAL_CAPACITY;
    graph->roots = (XrDepNode**)xr_malloc(graph->root_capacity * sizeof(XrDepNode*));
    
    if (!graph->nodes || !graph->roots) {
        xr_free(graph->nodes);
        xr_free(graph->roots);
        xr_free(graph);
        return NULL;
    }
    
    memset(graph->nodes, 0, graph->node_capacity * sizeof(XrDepNode*));
    memset(graph->roots, 0, graph->root_capacity * sizeof(XrDepNode*));
    
    return graph;
}

void xr_depgraph_free(XrDepGraph *graph) {
    if (!graph) return;
    
    for (int i = 0; i < graph->node_count; i++) {
        node_free(graph->nodes[i]);
    }
    
    xr_free(graph->nodes);
    xr_free(graph->roots);
    xr_free(graph);
}

/*
 * Get or create node.
 */
static XrDepNode* graph_get_or_create_node(XrDepGraph *graph, 
                                           const char *name,
                                           const char *constraint) {
    // Find existing node
    XrDepNode *node = xr_depgraph_find(graph, name);
    if (node) return node;
    
    // Create new node
    node = node_new(name, constraint);
    if (!node) return NULL;
    
    // Check capacity
    if (graph->node_count >= graph->node_capacity) {
        int old_cap = graph->node_capacity;
        int new_cap = old_cap * 2;
        XrDepNode **new_nodes = (XrDepNode**)xr_realloc(graph->nodes, new_cap * sizeof(XrDepNode*));
        if (!new_nodes) {
            node_free(node);
            return NULL;
        }
        graph->nodes = new_nodes;
        graph->node_capacity = new_cap;
    }
    
    graph->nodes[graph->node_count++] = node;
    return node;
}

bool xr_depgraph_add_root(XrDepGraph *graph, const char *name, 
                          const char *constraint) {
    if (!graph || !name) return false;
    
    XrDepNode *node = graph_get_or_create_node(graph, name, constraint);
    if (!node) return false;
    
    node->depth = 0;
    
    // Check capacity before adding to root list
    if (graph->root_count >= graph->root_capacity) {
        int old_cap = graph->root_capacity;
        int new_cap = old_cap * 2;
        XrDepNode **new_roots = (XrDepNode**)xr_realloc(graph->roots, new_cap * sizeof(XrDepNode*));
        if (!new_roots) return false;
        graph->roots = new_roots;
        graph->root_capacity = new_cap;
    }
    
    graph->roots[graph->root_count++] = node;
    return true;
}

XrDepNode* xr_depgraph_find(XrDepGraph *graph, const char *name) {
    if (!graph || !name) return NULL;
    
    for (int i = 0; i < graph->node_count; i++) {
        if (strcmp(graph->nodes[i]->name, name) == 0) {
            return graph->nodes[i];
        }
    }
    
    return NULL;
}

/* ========== Cycle Detection ========== */

#define CYCLE_PATH_BUF_SIZE 4096

/*
 * DFS cycle detection with dynamic path buffer.
 */
static bool detect_cycle_dfs(XrDepNode *node, char **cycle_path, 
                             char *path_buf, int path_len, int buf_size) {
    if (node->in_stack) {
        // Cycle found
        if (cycle_path && path_len + strlen(node->name) + 5 < (size_t)buf_size) {
            snprintf(path_buf + path_len, buf_size - path_len, 
                    " -> %s", node->name);
            *cycle_path = xr_strdup(path_buf);
        }
        return true;
    }
    
    if (node->visited) {
        return false;  // Already visited, no cycle
    }
    
    node->visited = true;
    node->in_stack = true;
    
    int new_len = path_len;
    int name_len = strlen(node->name);
    
    // Check buffer space before writing
    if (new_len + name_len + 8 < buf_size) {
        if (path_len > 0) {
            new_len += snprintf(path_buf + path_len, buf_size - path_len, " -> ");
        }
        new_len += snprintf(path_buf + new_len, buf_size - new_len, "%s", node->name);
    }
    
    for (int i = 0; i < node->dep_count; i++) {
        if (detect_cycle_dfs(node->dependencies[i], cycle_path, 
                            path_buf, new_len, buf_size)) {
            node->in_stack = false;
            return true;
        }
    }
    
    node->in_stack = false;
    return false;
}

bool xr_depgraph_has_cycle(XrDepGraph *graph, char **cycle_path) {
    if (!graph) return false;
    
    // Reset visit flags
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i]->visited = false;
        graph->nodes[i]->in_stack = false;
    }
    
    char *path_buf = (char*)xr_malloc(CYCLE_PATH_BUF_SIZE);
    if (!path_buf) return false;
    memset(path_buf, 0, CYCLE_PATH_BUF_SIZE);
    
    bool has_cycle = false;
    // Traverse all nodes, not just roots, to detect cycles in unreachable subgraphs
    for (int i = 0; i < graph->node_count; i++) {
        if (!graph->nodes[i]->visited) {
            if (detect_cycle_dfs(graph->nodes[i], cycle_path, path_buf, 0, 
                                CYCLE_PATH_BUF_SIZE)) {
                has_cycle = true;
                break;
            }
        }
    }
    
    xr_free(path_buf);
    return has_cycle;
}

/* ========== Topological Sort ========== */

/*
 * Topological sort DFS.
 */
static void topo_dfs(XrDepNode *node, XrDepNode **result, int *index) {
    if (node->visited) return;
    
    node->visited = true;
    
    // Visit all dependencies first
    for (int i = 0; i < node->dep_count; i++) {
        topo_dfs(node->dependencies[i], result, index);
    }
    
    // Then add self
    result[(*index)++] = node;
}

bool xr_depgraph_topo_sort(XrDepGraph *graph, XrDepNode ***order, int *count) {
    if (!graph || !order || !count) return false;
    
    // Check for cycle
    if (xr_depgraph_has_cycle(graph, NULL)) {
        return false;
    }
    
    // Reset visit flags
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i]->visited = false;
    }
    
    // Allocate result array
    *order = (XrDepNode**)xr_malloc(graph->node_count * sizeof(XrDepNode*));
    if (!*order) return false;
    memset(*order, 0, graph->node_count * sizeof(XrDepNode*));
    
    *count = 0;
    
    // Start DFS from all root nodes
    for (int i = 0; i < graph->root_count; i++) {
        topo_dfs(graph->roots[i], *order, count);
    }
    
    return true;
}

/* ========== Resolution API Implementation ========== */

/*
 * Recursively resolve dependencies.
 */
static bool resolve_node(XrDepGraph *graph, XrDepNode *node,
                         XrPackageInfoFn get_info,
                         const XrLockfile *lockfile,
                         void *user_data,
                         char **error) {
    if (node->resolved) return true;
    
    // 1. Check if version is pinned in lockfile
    if (lockfile) {
        const XrLockedPackage *locked = xr_lockfile_find(lockfile, node->name);
        if (locked && locked->version) {
            xr_semver_parse(locked->version, &node->resolved_version);
            node->resolved = true;
            
            // Add locked dependencies
            for (int i = 0; i < locked->dep_count; i++) {
                // Parse dependency spec "name@version"
                char *spec = xr_strdup(locked->dependencies[i]);
                char *at = strchr(spec, '@');
                char *dep_name = spec;
                char *dep_ver = "^1.0.0";
                
                if (at) {
                    *at = '\0';
                    dep_ver = at + 1;
                }
                
                XrDepNode *dep = graph_get_or_create_node(graph, dep_name, dep_ver);
                if (dep) {
                    dep->depth = node->depth + 1;
                    node_add_dep(node, dep);
                }
                
                xr_free(spec);
            }
            
            // Recursively resolve dependencies
            for (int i = 0; i < node->dep_count; i++) {
                if (!resolve_node(graph, node->dependencies[i], 
                                 get_info, lockfile, user_data, error)) {
                    return false;
                }
            }
            
            return true;
        }
    }
    
    // 2. Get package info from registry
    if (!get_info) {
        // Cannot get package info, mark as pending download
        node->resolved = true;
        return true;
    }
    
    XrPackageInfo *info = get_info(node->name, user_data);
    if (!info) {
        if (error) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Cannot get package info: %s", node->name);
            *error = xr_strdup(buf);
        }
        return false;
    }
    
    // 3. Select latest version satisfying constraint
    int best_idx = -1;
    XrSemVer best_ver = {0};
    
    for (int i = 0; i < info->version_count; i++) {
        XrSemVer ver;
        if (!xr_semver_parse(info->versions[i], &ver)) {
            continue;
        }
        
        if (xr_constraint_matches(&ver, &node->constraint)) {
            if (best_idx < 0 || xr_semver_compare(&ver, &best_ver) > 0) {
                xr_semver_free(&best_ver);
                best_ver = ver;
                best_idx = i;
            } else {
                xr_semver_free(&ver);
            }
        } else {
            xr_semver_free(&ver);
        }
    }
    
    if (best_idx < 0) {
        if (error) {
            char buf[256];
            snprintf(buf, sizeof(buf), 
                    "No version satisfying constraint %s: %s", 
                    node->constraint_str, node->name);
            *error = xr_strdup(buf);
        }
        xr_package_info_free(info);
        return false;
    }
    
    node->resolved_version = best_ver;
    node->resolved = true;
    
    // 4. Handle transitive dependencies
    for (int i = 0; i < info->dep_count; i++) {
        // Parse dependency spec
        char *spec = xr_strdup(info->deps[i]);
        char *at = strchr(spec, '@');
        char *dep_name = spec;
        char *dep_ver = "^1.0.0";
        
        if (at) {
            *at = '\0';
            dep_ver = at + 1;
        }
        
        XrDepNode *dep = graph_get_or_create_node(graph, dep_name, dep_ver);
        if (dep) {
            dep->depth = node->depth + 1;
            node_add_dep(node, dep);
        }
        
        xr_free(spec);
    }
    
    xr_package_info_free(info);
    
    // 5. Recursively resolve dependencies
    for (int i = 0; i < node->dep_count; i++) {
        if (!resolve_node(graph, node->dependencies[i], 
                         get_info, lockfile, user_data, error)) {
            return false;
        }
    }
    
    return true;
}

XrResolveResult* xr_resolve_dependencies(XrDepGraph *graph,
                                         XrPackageInfoFn get_info,
                                         const XrLockfile *lockfile,
                                         void *user_data) {
    XrResolveResult *result = (XrResolveResult*)xr_malloc(sizeof(XrResolveResult));
    if (!result) return NULL;
    memset(result, 0, sizeof(XrResolveResult));
    
    char *error = NULL;
    
    // 1. Resolve all root dependencies
    for (int i = 0; i < graph->root_count; i++) {
        if (!resolve_node(graph, graph->roots[i], 
                         get_info, lockfile, user_data, &error)) {
            result->success = false;
            result->error = error;
            return result;
        }
    }
    
    // 2. Detect circular dependencies
    char *cycle_path = NULL;
    if (xr_depgraph_has_cycle(graph, &cycle_path)) {
        result->success = false;
        result->error = cycle_path;
        return result;
    }
    
    // 3. Topological sort
    XrDepNode **order = NULL;
    int count = 0;
    
    if (!xr_depgraph_topo_sort(graph, &order, &count)) {
        result->success = false;
        result->error = xr_strdup("Topological sort failed");
        return result;
    }
    
    // 4. Generate result
    result->packages = (char**)xr_malloc(count * sizeof(char*));
    result->versions = (char**)xr_malloc(count * sizeof(char*));
    result->count = count;
    if (result->packages) memset(result->packages, 0, count * sizeof(char*));
    if (result->versions) memset(result->versions, 0, count * sizeof(char*));
    
    for (int i = 0; i < count; i++) {
        result->packages[i] = xr_strdup(order[i]->name);
        
        char ver_buf[64];
        xr_semver_to_string(&order[i]->resolved_version, ver_buf, sizeof(ver_buf));
        result->versions[i] = xr_strdup(ver_buf);
    }
    
    xr_free(order);
    
    result->success = true;
    return result;
}

void xr_resolve_result_free(XrResolveResult *result) {
    if (!result) return;
    
    for (int i = 0; i < result->count; i++) {
        xr_free(result->packages[i]);
        xr_free(result->versions[i]);
    }
    
    xr_free(result->packages);
    xr_free(result->versions);
    xr_free(result->error);
    xr_free(result);
}

/* ========== Debug API ========== */

/*
 * Recursively print node.
 */
#define MAX_PRINT_DEPTH 32

static void print_node(const XrDepNode *node, int indent) {
    if (indent > MAX_PRINT_DEPTH) {
        for (int i = 0; i < indent; i++) printf("  ");
        printf("... (depth limit reached)\n");
        return;
    }
    
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
    
    char ver_buf[64] = "unresolved";
    if (node->resolved) {
        xr_semver_to_string(&node->resolved_version, ver_buf, sizeof(ver_buf));
    }
    
    printf("%s@%s (%s)\n", node->name, ver_buf, 
           node->constraint_str ? node->constraint_str : "*");
    
    for (int i = 0; i < node->dep_count; i++) {
        print_node(node->dependencies[i], indent + 1);
    }
}

void xr_depgraph_print(const XrDepGraph *graph) {
    if (!graph) return;
    
    printf("Dependency tree:\n");
    for (int i = 0; i < graph->root_count; i++) {
        print_node(graph->roots[i], 0);
    }
}
