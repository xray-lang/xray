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
#include "../base/xhashmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 16
#define DEFAULT_CONSTRAINT "^1.0.0"

static void node_free(XrDepNode *node);

/*
 * Parse "name@version" dependency spec.
 * Sets *out_name to the name part (within the dup'd buffer) and
 * *out_ver to the version part (or DEFAULT_CONSTRAINT if no @).
 * Returns xr_malloc'd buffer that caller must xr_free().
 */
static char *parse_dep_spec(const char *spec, const char **out_name, const char **out_ver) {
    char *buf = xr_strdup(spec);
    if (!buf)
        return NULL;
    char *at = strchr(buf, '@');
    if (at) {
        *at = '\0';
        *out_name = buf;
        *out_ver = at + 1;
    } else {
        *out_name = buf;
        *out_ver = DEFAULT_CONSTRAINT;
    }
    return buf;
}

static XrDepNode *node_new(const char *name, const char *constraint_str) {
    XR_DCHECK(name != NULL, "node_new: NULL name");
    XrDepNode *node = (XrDepNode *) xr_malloc(sizeof(XrDepNode));
    if (!node)
        return NULL;
    memset(node, 0, sizeof(XrDepNode));

    node->name = xr_strdup(name);
    node->constraint_str = xr_strdup(constraint_str);

    XrOsIoPolicy policy = xr_os_io_system_policy();
    if (!node->name || (constraint_str && !node->constraint_str) ||
        (constraint_str && xr_constraint_parse_owned(&policy, constraint_str, &node->constraint) != XR_OS_IO_OK)) {
        node_free(node);
        return NULL;
    }

    node->dep_capacity = 8;
    node->dependencies = (XrDepNode **) xr_malloc(node->dep_capacity * sizeof(XrDepNode *));
    if (!node->dependencies) {
        node_free(node);
        return NULL;
    }
    memset(node->dependencies, 0, node->dep_capacity * sizeof(XrDepNode *));

    return node;
}

/*
 * Free dependency node.
 */
static void node_free(XrDepNode *node) {
    if (!node)
        return;

    xr_free(node->name);
    xr_free(node->constraint_str);
    xr_constraint_free_owned(&node->constraint);
    xr_semver_free_owned(&node->resolved_version);
    xr_free(node->dependencies);
    xr_free(node);
}

/*
 * Add dependency to node.
 */
static bool node_add_dep(XrDepNode *node, XrDepNode *dep) {
    if (!node || !dep)
        return false;

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
        XrDepNode **new_deps =
            (XrDepNode **) xr_realloc(node->dependencies, new_cap * sizeof(XrDepNode *));
        if (!new_deps)
            return false;
        node->dependencies = new_deps;
        node->dep_capacity = new_cap;
    }

    node->dependencies[node->dep_count++] = dep;
    XR_DCHECK(node->dep_count <= node->dep_capacity, "node_add_dep: count > capacity");
    return true;
}

/* ========== Package Info API ========== */

void xr_package_info_free(XrPackageInfo *info) {
    if (!info)
        return;

    xr_free(info->name);
    xr_free(info->latest_version);
    xr_free(info->description);

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

XrDepGraph *xr_depgraph_new(void) {
    XrDepGraph *graph = (XrDepGraph *) xr_malloc(sizeof(XrDepGraph));
    if (!graph)
        return NULL;
    memset(graph, 0, sizeof(XrDepGraph));

    graph->node_capacity = INITIAL_CAPACITY;
    graph->nodes = (XrDepNode **) xr_malloc(graph->node_capacity * sizeof(XrDepNode *));

    graph->root_capacity = INITIAL_CAPACITY;
    graph->roots = (XrDepNode **) xr_malloc(graph->root_capacity * sizeof(XrDepNode *));
    /* Package installation owns this graph independently of compiler state. */
    XrOsIoPolicy policy = xr_os_io_system_policy();
    XrOsIoStatus status = xr_hashmap_owned_new(&policy, &graph->index);

    if (!graph->nodes || !graph->roots || status != XR_OS_IO_OK) {
        xr_hashmap_owned_free(graph->index);
        xr_free(graph->nodes);
        xr_free(graph->roots);
        xr_free(graph);
        return NULL;
    }

    memset(graph->nodes, 0, graph->node_capacity * sizeof(XrDepNode *));
    memset(graph->roots, 0, graph->root_capacity * sizeof(XrDepNode *));

    return graph;
}

void xr_depgraph_free(XrDepGraph *graph) {
    if (!graph)
        return;

    /* The map borrows node names, which remain live through its destruction. */
    xr_hashmap_owned_free(graph->index);
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
static XrDepNode *graph_get_or_create_node(XrDepGraph *graph, const char *name,
                                           const char *constraint) {
    // Find existing node via hashmap O(1)
    XrDepNode *node = xr_depgraph_find(graph, name);
    if (node)
        return node;

    // Create new node
    node = node_new(name, constraint);
    if (!node)
        return NULL;

    // Check capacity
    if (graph->node_count >= graph->node_capacity) {
        int new_cap = graph->node_capacity * 2;
        XrDepNode **new_nodes =
            (XrDepNode **) xr_realloc(graph->nodes, new_cap * sizeof(XrDepNode *));
        if (!new_nodes) {
            node_free(node);
            return NULL;
        }
        graph->nodes = new_nodes;
        graph->node_capacity = new_cap;
    }

    graph->nodes[graph->node_count++] = node;
    if (xr_hashmap_owned_set(graph->index, node->name, node) != XR_OS_IO_OK) {
        // Unindexed nodes would be re-created on the next lookup; keep the
        // graph and index consistent by rolling back.
        graph->node_count--;
        node_free(node);
        return NULL;
    }
    return node;
}

bool xr_depgraph_add_root(XrDepGraph *graph, const char *name, const char *constraint) {
    if (!graph || !name)
        return false;

    XrDepNode *node = graph_get_or_create_node(graph, name, constraint);
    if (!node)
        return false;

    node->depth = 0;

    // Check capacity before adding to root list
    if (graph->root_count >= graph->root_capacity) {
        int old_cap = graph->root_capacity;
        int new_cap = old_cap * 2;
        XrDepNode **new_roots =
            (XrDepNode **) xr_realloc(graph->roots, new_cap * sizeof(XrDepNode *));
        if (!new_roots)
            return false;
        graph->roots = new_roots;
        graph->root_capacity = new_cap;
    }

    graph->roots[graph->root_count++] = node;
    return true;
}

XrDepNode *xr_depgraph_find(XrDepGraph *graph, const char *name) {
    if (!graph || !name)
        return NULL;
    void *value = NULL;
    if (xr_hashmap_owned_get(graph->index, name, &value) != XR_OS_IO_OK)
        return NULL;
    return value;
}

/* ========== Cycle Detection ========== */

#define CYCLE_PATH_BUF_SIZE 4096

/*
 * DFS cycle detection with dynamic path buffer.
 */
static bool detect_cycle_dfs(XrDepNode *node, char **cycle_path, char *path_buf, int path_len,
                             int buf_size) {
    if (node->in_stack) {
        // Cycle found
        if (cycle_path && path_len + strlen(node->name) + 5 < (size_t) buf_size) {
            snprintf(path_buf + path_len, buf_size - path_len, " -> %s", node->name);
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
        if (detect_cycle_dfs(node->dependencies[i], cycle_path, path_buf, new_len, buf_size)) {
            node->in_stack = false;
            return true;
        }
    }

    node->in_stack = false;
    return false;
}

bool xr_depgraph_has_cycle(XrDepGraph *graph, char **cycle_path) {
    if (!graph)
        return false;

    // Reset visit flags
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i]->visited = false;
        graph->nodes[i]->in_stack = false;
    }

    char *path_buf = (char *) xr_malloc(CYCLE_PATH_BUF_SIZE);
    if (!path_buf)
        return false;
    memset(path_buf, 0, CYCLE_PATH_BUF_SIZE);

    bool has_cycle = false;
    // Traverse all nodes, not just roots, to detect cycles in unreachable subgraphs
    for (int i = 0; i < graph->node_count; i++) {
        if (!graph->nodes[i]->visited) {
            if (detect_cycle_dfs(graph->nodes[i], cycle_path, path_buf, 0, CYCLE_PATH_BUF_SIZE)) {
                has_cycle = true;
                break;
            }
        }
    }

    xr_free(path_buf);
    return has_cycle;
}

/* ========== Topological Sort (with inline cycle detection) ========== */

/*
 * Single-pass DFS: cycle detection + topological ordering.
 * Returns false if a cycle is detected.
 */
static bool topo_dfs(XrDepNode *node, XrDepNode **result, int *index, bool *has_cycle) {
    if (node->in_stack) {
        *has_cycle = true;
        return false;
    }
    if (node->visited)
        return true;

    node->visited = true;
    node->in_stack = true;

    for (int i = 0; i < node->dep_count; i++) {
        if (!topo_dfs(node->dependencies[i], result, index, has_cycle)) {
            node->in_stack = false;
            return false;
        }
    }

    node->in_stack = false;
    result[(*index)++] = node;
    return true;
}

bool xr_depgraph_topo_sort(XrDepGraph *graph, XrDepNode ***order, int *count) {
    if (!graph || !order || !count)
        return false;

    // Reset visit flags
    for (int i = 0; i < graph->node_count; i++) {
        graph->nodes[i]->visited = false;
        graph->nodes[i]->in_stack = false;
    }

    // Allocate result array
    *order = (XrDepNode **) xr_malloc(graph->node_count * sizeof(XrDepNode *));
    if (!*order)
        return false;
    memset(*order, 0, graph->node_count * sizeof(XrDepNode *));

    *count = 0;
    bool has_cycle = false;

    // Single-pass DFS from root nodes: detect cycles + topo sort
    for (int i = 0; i < graph->root_count; i++) {
        if (!topo_dfs(graph->roots[i], *order, count, &has_cycle)) {
            xr_free(*order);
            *order = NULL;
            *count = 0;
            return false;
        }
    }

    return true;
}

/* ========== Resolution API Implementation ========== */

/*
 * Recursively resolve dependencies.
 */
static bool resolve_node(XrDepGraph *graph, XrDepNode *node, XrPackageInfoFn get_info,
                         const XrLockfile *lockfile, void *user_data, char **error) {
    if (node->resolved)
        return true;
    XrOsIoPolicy policy = xr_os_io_system_policy();

    // 1. Check if version is pinned in lockfile
    if (lockfile) {
        const XrLockedPackage *locked = NULL;
        XrOsIoStatus status = xr_lockfile_find_owned(&lockfile->policy, lockfile, node->name, &locked);
        if (status != XR_OS_IO_OK && status != XR_OS_IO_NOT_FOUND)
            return false;
        if (locked && locked->version) {
            xr_semver_free_owned(&node->resolved_version);
            if (xr_semver_parse_owned(&policy, locked->version, &node->resolved_version) != XR_OS_IO_OK)
                return false;
            node->resolved = true;

            // Add locked dependencies
            for (int i = 0; i < locked->dep_count; i++) {
                const char *dep_name, *dep_ver;
                char *buf = parse_dep_spec(locked->dependencies[i], &dep_name, &dep_ver);
                if (!buf) {
                    node->resolved = false;
                    return false;
                }
                XrDepNode *dep = graph_get_or_create_node(graph, dep_name, dep_ver);
                bool added = dep && node_add_dep(node, dep);
                if (added)
                    dep->depth = node->depth + 1;
                xr_free(buf);
                if (!added) {
                    node->resolved = false;
                    return false;
                }
            }

            // Recursively resolve dependencies
            for (int i = 0; i < node->dep_count; i++) {
                if (!resolve_node(graph, node->dependencies[i], get_info, lockfile, user_data,
                                  error)) {
                    node->resolved = false;
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
        XrSemVer ver = {0};
        XrOsIoStatus status = xr_semver_parse_owned(&policy, info->versions[i], &ver);
        if (status == XR_OS_IO_BAD_ARGUMENT)
            continue;
        if (status != XR_OS_IO_OK)
            goto version_failure;
        bool matches = false;
        status = xr_constraint_matches_owned(&policy, &ver, &node->constraint, &matches);
        int comparison = 0;
        if (status == XR_OS_IO_OK && matches && best_idx >= 0)
            status = xr_semver_compare_owned(&policy, &ver, &best_ver, &comparison);
        if (status != XR_OS_IO_OK) {
            xr_semver_free_owned(&ver);
            goto version_failure;
        }
        if (matches && (best_idx < 0 || comparison > 0)) {
            xr_semver_free_owned(&best_ver);
            best_ver = ver;
            best_idx = i;
        } else
            xr_semver_free_owned(&ver);
    }

    if (best_idx < 0) {
        if (error) {
            char buf[256];
            snprintf(buf, sizeof(buf), "No version satisfying constraint %s: %s",
                     node->constraint_str, node->name);
            *error = xr_strdup(buf);
        }
        xr_package_info_free(info);
        return false;
    }

    xr_semver_free_owned(&node->resolved_version);
    node->resolved_version = best_ver;
    node->resolved = true;

    // 4. Handle transitive dependencies
    for (int i = 0; i < info->dep_count; i++) {
        const char *dep_name, *dep_ver;
        char *buf = parse_dep_spec(info->deps[i], &dep_name, &dep_ver);
        XrDepNode *dep = buf ? graph_get_or_create_node(graph, dep_name, dep_ver) : NULL;
        bool added = dep && node_add_dep(node, dep);
        if (added)
            dep->depth = node->depth + 1;
        xr_free(buf);
        if (!added) {
            node->resolved = false;
            xr_package_info_free(info);
            return false;
        }
    }

    xr_package_info_free(info);

    // 5. Recursively resolve dependencies
    for (int i = 0; i < node->dep_count; i++) {
        if (!resolve_node(graph, node->dependencies[i], get_info, lockfile, user_data, error)) {
            node->resolved = false;
            return false;
        }
    }

    return true;
version_failure:
    xr_semver_free_owned(&best_ver);
    xr_package_info_free(info);
    return false;
}

XrResolveResult *xr_resolve_dependencies(XrDepGraph *graph, XrPackageInfoFn get_info,
                                         const XrLockfile *lockfile, void *user_data) {
    XrResolveResult *result = (XrResolveResult *) xr_malloc(sizeof(XrResolveResult));
    if (!result)
        return NULL;
    memset(result, 0, sizeof(XrResolveResult));

    char *error = NULL;

    // 1. Resolve all root dependencies
    for (int i = 0; i < graph->root_count; i++) {
        if (!resolve_node(graph, graph->roots[i], get_info, lockfile, user_data, &error)) {
            result->success = false;
            result->error = error ? error : xr_strdup("Dependency metadata operation failed");
            return result;
        }
    }

    // 2. Topological sort (includes inline cycle detection)
    XrDepNode **order = NULL;
    int count = 0;

    if (!xr_depgraph_topo_sort(graph, &order, &count)) {
        result->success = false;
        result->error = xr_strdup("Circular dependency or topological sort failed");
        return result;
    }

    // 4. Generate result
    result->packages = (char **) xr_calloc((size_t) count, sizeof(char *));
    result->versions = (char **) xr_calloc((size_t) count, sizeof(char *));
    if (count > 0 && (!result->packages || !result->versions)) {
        xr_free(order);
        xr_resolve_result_free(result);
        return NULL;
    }
    result->count = count;
    if (result->packages)
        memset(result->packages, 0, count * sizeof(char *));
    if (result->versions)
        memset(result->versions, 0, count * sizeof(char *));

    for (int i = 0; i < count; i++) {
        result->packages[i] = xr_strdup(order[i]->name);

        char ver_buf[64];
        XrOsIoPolicy policy = xr_os_io_system_policy();
        size_t written = 0;
        if (xr_semver_to_string_owned(&policy, &order[i]->resolved_version, ver_buf, sizeof(ver_buf), &written) != XR_OS_IO_OK) {
            xr_free(order);
            xr_resolve_result_free(result);
            return NULL;
        }
        result->versions[i] = xr_strdup(ver_buf);
        if (!result->packages[i] || !result->versions[i]) {
            xr_free(order);
            xr_resolve_result_free(result);
            return NULL;
        }
    }

    xr_free(order);

    result->success = true;
    return result;
}

void xr_resolve_result_free(XrResolveResult *result) {
    if (!result)
        return;

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
        for (int i = 0; i < indent; i++)
            printf("  ");
        printf("... (depth limit reached)\n");
        return;
    }

    for (int i = 0; i < indent; i++) {
        printf("  ");
    }

    char ver_buf[64] = "unresolved";
    if (node->resolved) {
        XrOsIoPolicy policy = xr_os_io_system_policy();
        size_t written = 0;
        if (xr_semver_to_string_owned(&policy, &node->resolved_version, ver_buf, sizeof(ver_buf), &written) != XR_OS_IO_OK)
            memcpy(ver_buf, "unprintable", sizeof("unprintable"));
    }

    printf("%s@%s (%s)\n", node->name, ver_buf, node->constraint_str ? node->constraint_str : "*");

    for (int i = 0; i < node->dep_count; i++) {
        print_node(node->dependencies[i], indent + 1);
    }
}

void xr_depgraph_print(const XrDepGraph *graph) {
    if (!graph)
        return;

    printf("Dependency tree:\n");
    for (int i = 0; i < graph->root_count; i++) {
        print_node(graph->roots[i], 0);
    }
}
