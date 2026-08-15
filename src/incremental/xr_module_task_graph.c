/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_task_graph.c - Deterministic module SCC task scheduling
 */

#include "xr_module_task_graph.h"

#include "../base/xmalloc.h"
#include "../base/xsha256.h"
#include "../os/os_thread.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_MODULE_TASK_MAX_WORKERS 64u

typedef struct XrModuleTaskRow {
    uint32_t level;
    uint32_t member_begin;
    uint32_t member_count;
    uint32_t dependency_begin;
    uint32_t dependency_count;
} XrModuleTaskRow;

struct XrModuleTaskGraph {
    XrFingerprint dependency_graph_fingerprint;
    XrFingerprint fingerprint;
    XrModuleTaskRow *tasks;
    XrStableId *members;
    uint32_t *dependencies;
    uint32_t task_count;
    uint32_t member_count;
    uint32_t dependency_count;
};

typedef struct XrComponentEdge {
    uint32_t consumer;
    uint32_t dependency;
} XrComponentEdge;

typedef struct XrTarjanState {
    const uint32_t *offsets;
    const uint32_t *adjacency;
    int32_t *indices;
    uint32_t *lowlinks;
    uint32_t *stack;
    uint32_t *components;
    uint8_t *on_stack;
    uint32_t next_index;
    uint32_t stack_count;
    uint32_t component_count;
} XrTarjanState;

typedef struct XrModuleTaskPool {
    const XrModuleTaskGraph *graph;
    const uint32_t *ready;
    uint32_t ready_count;
    uint32_t worker_count;
    XrModuleTaskExecuteFn execute;
    void *context;
    uint8_t *task_states;
    size_t task_state_size;
    XrModuleTaskOutput *outputs;
} XrModuleTaskPool;

typedef struct XrModuleTaskWorker {
    XrModuleTaskPool *pool;
    uint32_t ordinal;
    atomic_bool finished;
} XrModuleTaskWorker;

static void set_error(char *error, size_t error_size, const char *format, ...) {
    if (!error || error_size == 0)
        return;
    va_list args;
    va_start(args, format);
    (void) vsnprintf(error, error_size, format, args);
    va_end(args);
    error[error_size - 1u] = '\0';
}

static void hash_u32(XrSHA256Context *ctx, uint32_t value) {
    uint8_t bytes[4];
    for (unsigned i = 0; i < sizeof(bytes); i++)
        bytes[i] = (uint8_t) (value >> (i * 8u));
    xr_sha256_update(ctx, bytes, sizeof(bytes));
}

static int compare_stable_ids(const void *left, const void *right) {
    return xr_stable_id_compare(*(const XrStableId *) left,
                                *(const XrStableId *) right);
}

static int compare_u32(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *) left;
    uint32_t b = *(const uint32_t *) right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static int compare_component_edges(const void *left, const void *right) {
    const XrComponentEdge *a = (const XrComponentEdge *) left;
    const XrComponentEdge *b = (const XrComponentEdge *) right;
    if (a->consumer != b->consumer)
        return a->consumer < b->consumer ? -1 : 1;
    return a->dependency < b->dependency
               ? -1
               : a->dependency > b->dependency ? 1 : 0;
}

static uint32_t find_sorted_module(const XrStableId *modules,
                                   uint32_t module_count, XrStableId module) {
    uint32_t low = 0;
    uint32_t high = module_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        int order = xr_stable_id_compare(modules[middle], module);
        if (order < 0)
            low = middle + 1u;
        else
            high = middle;
    }
    return low < module_count && xr_stable_id_equal(modules[low], module)
               ? low
               : UINT32_MAX;
}

static void tarjan_visit(XrTarjanState *state, uint32_t node) {
    uint32_t index = state->next_index++;
    state->indices[node] = (int32_t) index;
    state->lowlinks[node] = index;
    state->stack[state->stack_count++] = node;
    state->on_stack[node] = 1;

    for (uint32_t edge = state->offsets[node];
         edge < state->offsets[node + 1u]; edge++) {
        uint32_t dependency = state->adjacency[edge];
        if (state->indices[dependency] < 0) {
            tarjan_visit(state, dependency);
            if (state->lowlinks[dependency] < state->lowlinks[node])
                state->lowlinks[node] = state->lowlinks[dependency];
        } else if (state->on_stack[dependency] &&
                   (uint32_t) state->indices[dependency] <
                       state->lowlinks[node]) {
            state->lowlinks[node] = (uint32_t) state->indices[dependency];
        }
    }

    if (state->lowlinks[node] != (uint32_t) state->indices[node])
        return;
    for (;;) {
        uint32_t member = state->stack[--state->stack_count];
        state->on_stack[member] = 0;
        state->components[member] = state->component_count;
        if (member == node)
            break;
    }
    state->component_count++;
}

static bool fingerprint_graph(const XrModuleTaskGraph *graph,
                              XrFingerprint *out) {
    static const uint8_t domain[] = "xray-module-task-graph-v1\0";
    if (!graph || !out)
        return false;
    XrSHA256Context ctx;
    xr_sha256_init(&ctx);
    xr_sha256_update(&ctx, domain, sizeof(domain) - 1u);
    xr_sha256_update(&ctx, graph->dependency_graph_fingerprint.bytes,
                     sizeof(graph->dependency_graph_fingerprint.bytes));
    hash_u32(&ctx, graph->task_count);
    hash_u32(&ctx, graph->member_count);
    hash_u32(&ctx, graph->dependency_count);
    for (uint32_t task = 0; task < graph->task_count; task++) {
        const XrModuleTaskRow *row = &graph->tasks[task];
        hash_u32(&ctx, row->level);
        hash_u32(&ctx, row->member_count);
        for (uint32_t member = 0; member < row->member_count; member++) {
            const XrStableId *id =
                &graph->members[row->member_begin + member];
            xr_sha256_update(&ctx, id->bytes, sizeof(id->bytes));
        }
        hash_u32(&ctx, row->dependency_count);
        for (uint32_t dependency = 0;
             dependency < row->dependency_count; dependency++) {
            hash_u32(&ctx, graph->dependencies[
                               row->dependency_begin + dependency]);
        }
    }
    xr_sha256_final(&ctx, out->bytes);
    return true;
}

static bool graph_header_valid(const XrModuleTaskGraph *graph) {
    return graph && graph->task_count <= XR_DEPENDENCY_GRAPH_MAX_NODES &&
           graph->member_count <= XR_DEPENDENCY_GRAPH_MAX_NODES &&
           graph->dependency_count <= XR_DEPENDENCY_GRAPH_MAX_EDGES &&
           (graph->task_count == 0 || graph->tasks) &&
           (graph->member_count == 0 || graph->members) &&
           (graph->dependency_count == 0 || graph->dependencies) &&
           graph->task_count <= graph->member_count;
}

static bool graph_integrity(const XrModuleTaskGraph *graph) {
    if (!graph_header_valid(graph))
        return false;

    uint32_t member_cursor = 0;
    uint32_t dependency_cursor = 0;
    for (uint32_t task = 0; task < graph->task_count; task++) {
        const XrModuleTaskRow *row = &graph->tasks[task];
        if (row->member_count == 0 || row->member_begin != member_cursor ||
            row->dependency_begin != dependency_cursor ||
            row->member_count > graph->member_count - member_cursor ||
            row->dependency_count >
                graph->dependency_count - dependency_cursor)
            return false;
        for (uint32_t member = 1; member < row->member_count; member++) {
            if (xr_stable_id_compare(
                    graph->members[row->member_begin + member - 1u],
                    graph->members[row->member_begin + member]) >= 0)
                return false;
        }
        uint32_t expected_level = 0;
        for (uint32_t dependency = 0;
             dependency < row->dependency_count; dependency++) {
            uint32_t dependency_task = graph->dependencies[
                row->dependency_begin + dependency];
            if (dependency_task >= task ||
                (dependency != 0 &&
                 graph->dependencies[row->dependency_begin + dependency - 1u] >=
                     dependency_task))
                return false;
            uint32_t level = graph->tasks[dependency_task].level + 1u;
            if (level > expected_level)
                expected_level = level;
        }
        if (row->level != expected_level)
            return false;
        member_cursor += row->member_count;
        dependency_cursor += row->dependency_count;
    }
    if (member_cursor != graph->member_count ||
        dependency_cursor != graph->dependency_count)
        return false;
    XrStableId *sorted = graph->member_count
                             ? (XrStableId *) xr_malloc(
                                   graph->member_count * sizeof(*sorted))
                             : NULL;
    if (graph->member_count && !sorted)
        return false;
    if (graph->member_count) {
        memcpy(sorted, graph->members,
               graph->member_count * sizeof(*sorted));
        qsort(sorted, graph->member_count, sizeof(*sorted),
              compare_stable_ids);
    }
    for (uint32_t member = 1; member < graph->member_count; member++) {
        if (xr_stable_id_equal(sorted[member - 1u], sorted[member])) {
            xr_free(sorted);
            return false;
        }
    }
    xr_free(sorted);
    XrFingerprint actual;
    return fingerprint_graph(graph, &actual) &&
           xr_fingerprint_equal(actual, graph->fingerprint);
}

static bool task_graphs_equal(const XrModuleTaskGraph *left,
                              const XrModuleTaskGraph *right) {
    if (!left || !right || left->task_count != right->task_count ||
        left->member_count != right->member_count ||
        left->dependency_count != right->dependency_count ||
        !xr_fingerprint_equal(left->dependency_graph_fingerprint,
                              right->dependency_graph_fingerprint) ||
        !xr_fingerprint_equal(left->fingerprint, right->fingerprint))
        return false;
    for (uint32_t task = 0; task < left->task_count; task++) {
        const XrModuleTaskRow *a = &left->tasks[task];
        const XrModuleTaskRow *b = &right->tasks[task];
        if (a->level != b->level || a->member_begin != b->member_begin ||
            a->member_count != b->member_count ||
            a->dependency_begin != b->dependency_begin ||
            a->dependency_count != b->dependency_count)
            return false;
    }
    for (uint32_t member = 0; member < left->member_count; member++)
        if (!xr_stable_id_equal(left->members[member], right->members[member]))
            return false;
    return left->dependency_count == 0 ||
           memcmp(left->dependencies, right->dependencies,
                  left->dependency_count * sizeof(*left->dependencies)) == 0;
}

void xr_module_task_graph_free(XrModuleTaskGraph *graph) {
    if (!graph)
        return;
    xr_free(graph->dependencies);
    xr_free(graph->members);
    xr_free(graph->tasks);
    xr_free(graph);
}

bool xr_module_task_graph_build(const XrDependencyGraph *dependencies,
                                XrModuleTaskGraph **out, char *error,
                                size_t error_size) {
    if (out)
        *out = NULL;
    if (error && error_size)
        error[0] = '\0';
    if (!out || !xr_dependency_graph_validate(dependencies) ||
        dependencies->node_count > UINT32_MAX ||
        dependencies->edge_count > UINT32_MAX) {
        set_error(error, error_size,
                  "module task graph requires a valid dependency graph");
        return false;
    }

    uint32_t node_count = (uint32_t) dependencies->node_count;
    uint32_t edge_count = (uint32_t) dependencies->edge_count;
    XrModuleTaskGraph *graph =
        (XrModuleTaskGraph *) xr_calloc(1, sizeof(*graph));
    XrStableId *modules = node_count
                              ? (XrStableId *) xr_malloc(
                                    node_count * sizeof(*modules))
                              : NULL;
    uint32_t *offsets = node_count
                            ? (uint32_t *) xr_calloc(
                                  (size_t) node_count + 1u, sizeof(*offsets))
                            : NULL;
    uint32_t *adjacency = edge_count
                              ? (uint32_t *) xr_malloc(
                                    edge_count * sizeof(*adjacency))
                              : NULL;
    uint32_t *positions = node_count
                              ? (uint32_t *) xr_malloc(
                                    node_count * sizeof(*positions))
                              : NULL;
    if (!graph || (node_count && (!modules || !offsets || !positions)) ||
        (edge_count && !adjacency)) {
        set_error(error, error_size,
                  "module task graph allocation failed");
        goto fail;
    }
    if (!xr_dependency_graph_fingerprint(
            dependencies, &graph->dependency_graph_fingerprint)) {
        set_error(error, error_size,
                  "module task graph source identity failed");
        goto fail;
    }
    for (uint32_t node = 0; node < node_count; node++)
        modules[node] = dependencies->nodes[node].module_id;
    if (node_count)
        qsort(modules, node_count, sizeof(*modules), compare_stable_ids);

    for (uint32_t edge = 0; edge < edge_count; edge++) {
        uint32_t consumer = find_sorted_module(
            modules, node_count, dependencies->edges[edge].consumer);
        if (consumer == UINT32_MAX || offsets[consumer + 1u] == UINT32_MAX) {
            set_error(error, error_size,
                      "module task graph edge authority is invalid");
            goto fail;
        }
        offsets[consumer + 1u]++;
    }
    for (uint32_t node = 0; node < node_count; node++) {
        if (offsets[node + 1u] > UINT32_MAX - offsets[node]) {
            set_error(error, error_size,
                      "module task graph adjacency overflowed");
            goto fail;
        }
        offsets[node + 1u] += offsets[node];
        positions[node] = offsets[node];
    }
    for (uint32_t edge = 0; edge < edge_count; edge++) {
        uint32_t consumer = find_sorted_module(
            modules, node_count, dependencies->edges[edge].consumer);
        uint32_t dependency = find_sorted_module(
            modules, node_count, dependencies->edges[edge].dependency);
        if (consumer == UINT32_MAX || dependency == UINT32_MAX) {
            set_error(error, error_size,
                      "module task graph edge endpoint is missing");
            goto fail;
        }
        adjacency[positions[consumer]++] = dependency;
    }
    for (uint32_t node = 0; node < node_count; node++) {
        uint32_t count = offsets[node + 1u] - offsets[node];
        if (count > 1u)
            qsort(&adjacency[offsets[node]], count, sizeof(*adjacency),
                  compare_u32);
    }

    int32_t *indices = node_count
                           ? (int32_t *) xr_malloc(node_count * sizeof(*indices))
                           : NULL;
    uint32_t *lowlinks = node_count
                             ? (uint32_t *) xr_malloc(
                                   node_count * sizeof(*lowlinks))
                             : NULL;
    uint32_t *stack = node_count
                          ? (uint32_t *) xr_malloc(node_count * sizeof(*stack))
                          : NULL;
    uint32_t *components = node_count
                               ? (uint32_t *) xr_malloc(
                                     node_count * sizeof(*components))
                               : NULL;
    uint8_t *on_stack = node_count
                            ? (uint8_t *) xr_calloc(node_count,
                                                   sizeof(*on_stack))
                            : NULL;
    if (node_count &&
        (!indices || !lowlinks || !stack || !components || !on_stack)) {
        xr_free(on_stack);
        xr_free(components);
        xr_free(stack);
        xr_free(lowlinks);
        xr_free(indices);
        set_error(error, error_size,
                  "module SCC allocation failed");
        goto fail;
    }
    for (uint32_t node = 0; node < node_count; node++)
        indices[node] = -1;
    XrTarjanState tarjan = {
        .offsets = offsets,
        .adjacency = adjacency,
        .indices = indices,
        .lowlinks = lowlinks,
        .stack = stack,
        .components = components,
        .on_stack = on_stack,
    };
    for (uint32_t node = 0; node < node_count; node++)
        if (indices[node] < 0)
            tarjan_visit(&tarjan, node);
    uint32_t component_count = tarjan.component_count;
    xr_free(on_stack);
    xr_free(stack);
    xr_free(lowlinks);
    xr_free(indices);

    XrStableId *component_keys = component_count
                                     ? (XrStableId *) xr_calloc(
                                           component_count,
                                           sizeof(*component_keys))
                                     : NULL;
    uint8_t *component_has_key = component_count
                                     ? (uint8_t *) xr_calloc(
                                           component_count,
                                           sizeof(*component_has_key))
                                     : NULL;
    XrComponentEdge *component_edges = edge_count
                                           ? (XrComponentEdge *) xr_malloc(
                                                 edge_count *
                                                 sizeof(*component_edges))
                                           : NULL;
    if ((component_count && (!component_keys || !component_has_key)) ||
        (edge_count && !component_edges)) {
        xr_free(component_edges);
        xr_free(component_has_key);
        xr_free(component_keys);
        xr_free(components);
        set_error(error, error_size,
                  "module component identity allocation failed");
        goto fail;
    }
    for (uint32_t node = 0; node < node_count; node++) {
        uint32_t component = components[node];
        if (!component_has_key[component] ||
            xr_stable_id_compare(modules[node], component_keys[component]) < 0) {
            component_keys[component] = modules[node];
            component_has_key[component] = 1;
        }
    }
    uint32_t component_edge_count = 0;
    for (uint32_t edge = 0; edge < edge_count; edge++) {
        uint32_t consumer = find_sorted_module(
            modules, node_count, dependencies->edges[edge].consumer);
        uint32_t dependency = find_sorted_module(
            modules, node_count, dependencies->edges[edge].dependency);
        uint32_t consumer_component = components[consumer];
        uint32_t dependency_component = components[dependency];
        if (consumer_component != dependency_component) {
            component_edges[component_edge_count++] = (XrComponentEdge) {
                .consumer = consumer_component,
                .dependency = dependency_component,
            };
        }
    }
    if (component_edge_count > 1u)
        qsort(component_edges, component_edge_count,
              sizeof(*component_edges), compare_component_edges);
    uint32_t unique_edge_count = 0;
    for (uint32_t edge = 0; edge < component_edge_count; edge++) {
        if (unique_edge_count == 0 ||
            compare_component_edges(&component_edges[unique_edge_count - 1u],
                                    &component_edges[edge]) != 0) {
            component_edges[unique_edge_count++] = component_edges[edge];
        }
    }
    component_edge_count = unique_edge_count;

    uint32_t *indegree = component_count
                             ? (uint32_t *) xr_calloc(component_count,
                                                      sizeof(*indegree))
                             : NULL;
    uint32_t *component_to_task = component_count
                                      ? (uint32_t *) xr_malloc(
                                            component_count *
                                            sizeof(*component_to_task))
                                      : NULL;
    uint8_t *selected = component_count
                            ? (uint8_t *) xr_calloc(component_count,
                                                   sizeof(*selected))
                            : NULL;
    if (component_count &&
        (!indegree || !component_to_task || !selected)) {
        xr_free(selected);
        xr_free(component_to_task);
        xr_free(indegree);
        xr_free(component_edges);
        xr_free(component_has_key);
        xr_free(component_keys);
        xr_free(components);
        set_error(error, error_size,
                  "module task ordering allocation failed");
        goto fail;
    }
    for (uint32_t edge = 0; edge < component_edge_count; edge++)
        indegree[component_edges[edge].consumer]++;
    for (uint32_t task = 0; task < component_count; task++) {
        uint32_t best = UINT32_MAX;
        for (uint32_t component = 0; component < component_count; component++) {
            if (!selected[component] && indegree[component] == 0 &&
                (best == UINT32_MAX ||
                 xr_stable_id_compare(component_keys[component],
                                      component_keys[best]) < 0)) {
                best = component;
            }
        }
        if (best == UINT32_MAX) {
            xr_free(selected);
            xr_free(component_to_task);
            xr_free(indegree);
            xr_free(component_edges);
            xr_free(component_has_key);
            xr_free(component_keys);
            xr_free(components);
            set_error(error, error_size,
                      "module component graph is cyclic");
            goto fail;
        }
        selected[best] = 1;
        component_to_task[best] = task;
        for (uint32_t edge = 0; edge < component_edge_count; edge++)
            if (component_edges[edge].dependency == best)
                indegree[component_edges[edge].consumer]--;
    }
    xr_free(selected);
    xr_free(indegree);
    xr_free(component_has_key);
    xr_free(component_keys);

    graph->task_count = component_count;
    graph->member_count = node_count;
    graph->dependency_count = component_edge_count;
    graph->tasks = component_count
                       ? (XrModuleTaskRow *) xr_calloc(component_count,
                                                      sizeof(*graph->tasks))
                       : NULL;
    graph->members = node_count
                         ? (XrStableId *) xr_malloc(
                               node_count * sizeof(*graph->members))
                         : NULL;
    graph->dependencies = component_edge_count
                              ? (uint32_t *) xr_malloc(
                                    component_edge_count *
                                    sizeof(*graph->dependencies))
                              : NULL;
    uint32_t *member_positions = component_count
                                     ? (uint32_t *) xr_calloc(
                                           component_count,
                                           sizeof(*member_positions))
                                     : NULL;
    uint32_t *dependency_positions = component_count
                                         ? (uint32_t *) xr_calloc(
                                               component_count,
                                               sizeof(*dependency_positions))
                                         : NULL;
    if ((component_count &&
         (!graph->tasks || !member_positions || !dependency_positions)) ||
        (node_count && !graph->members) ||
        (component_edge_count && !graph->dependencies)) {
        xr_free(dependency_positions);
        xr_free(member_positions);
        xr_free(component_to_task);
        xr_free(component_edges);
        xr_free(components);
        set_error(error, error_size,
                  "module task row allocation failed");
        goto fail;
    }
    for (uint32_t node = 0; node < node_count; node++)
        graph->tasks[component_to_task[components[node]]].member_count++;
    for (uint32_t edge = 0; edge < component_edge_count; edge++)
        graph->tasks[component_to_task[component_edges[edge].consumer]]
            .dependency_count++;
    uint32_t member_begin = 0;
    uint32_t dependency_begin = 0;
    for (uint32_t task = 0; task < component_count; task++) {
        graph->tasks[task].member_begin = member_begin;
        graph->tasks[task].dependency_begin = dependency_begin;
        member_positions[task] = member_begin;
        dependency_positions[task] = dependency_begin;
        member_begin += graph->tasks[task].member_count;
        dependency_begin += graph->tasks[task].dependency_count;
    }
    for (uint32_t node = 0; node < node_count; node++) {
        uint32_t task = component_to_task[components[node]];
        graph->members[member_positions[task]++] = modules[node];
    }
    for (uint32_t edge = 0; edge < component_edge_count; edge++) {
        uint32_t consumer =
            component_to_task[component_edges[edge].consumer];
        uint32_t dependency =
            component_to_task[component_edges[edge].dependency];
        if (dependency >= consumer) {
            xr_free(dependency_positions);
            xr_free(member_positions);
            xr_free(component_to_task);
            xr_free(component_edges);
            xr_free(components);
            set_error(error, error_size,
                      "module task dependency order is invalid");
            goto fail;
        }
        graph->dependencies[dependency_positions[consumer]++] = dependency;
    }
    for (uint32_t task = 0; task < component_count; task++) {
        XrModuleTaskRow *row = &graph->tasks[task];
        if (row->dependency_count > 1u)
            qsort(&graph->dependencies[row->dependency_begin],
                  row->dependency_count, sizeof(*graph->dependencies),
                  compare_u32);
        for (uint32_t dependency = 0;
             dependency < row->dependency_count; dependency++) {
            uint32_t dependency_task = graph->dependencies[
                row->dependency_begin + dependency];
            uint32_t level = graph->tasks[dependency_task].level + 1u;
            if (level > row->level)
                row->level = level;
        }
    }
    xr_free(dependency_positions);
    xr_free(member_positions);
    xr_free(component_to_task);
    xr_free(component_edges);
    xr_free(components);

    if (!fingerprint_graph(graph, &graph->fingerprint) ||
        !graph_integrity(graph)) {
        set_error(error, error_size,
                  "module task graph failed self verification");
        goto fail;
    }
    xr_free(positions);
    xr_free(adjacency);
    xr_free(offsets);
    xr_free(modules);
    *out = graph;
    return true;

fail:
    xr_free(positions);
    xr_free(adjacency);
    xr_free(offsets);
    xr_free(modules);
    xr_module_task_graph_free(graph);
    return false;
}

bool xr_module_task_graph_verify(const XrDependencyGraph *dependencies,
                                 const XrModuleTaskGraph *graph, char *error,
                                 size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (!graph_integrity(graph)) {
        set_error(error, error_size,
                  "module task graph integrity is invalid");
        return false;
    }
    XrModuleTaskGraph *expected = NULL;
    if (!xr_module_task_graph_build(dependencies, &expected, error, error_size))
        return false;
    bool verified = task_graphs_equal(expected, graph);
    xr_module_task_graph_free(expected);
    if (!verified)
        set_error(error, error_size,
                  "module task graph does not match dependency authority");
    return verified;
}

uint32_t xr_module_task_graph_count(const XrModuleTaskGraph *graph) {
    return graph_header_valid(graph) ? graph->task_count : 0;
}

bool xr_module_task_graph_task(const XrModuleTaskGraph *graph,
                               uint32_t task_index, XrModuleTaskView *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !graph_header_valid(graph) || task_index >= graph->task_count)
        return false;
    const XrModuleTaskRow *row = &graph->tasks[task_index];
    if (row->member_begin > graph->member_count ||
        row->member_count > graph->member_count - row->member_begin ||
        row->dependency_begin > graph->dependency_count ||
        row->dependency_count >
            graph->dependency_count - row->dependency_begin)
        return false;
    *out = (XrModuleTaskView) {
        .level = row->level,
        .members = &graph->members[row->member_begin],
        .member_count = row->member_count,
        .dependencies = row->dependency_count
                            ? &graph->dependencies[row->dependency_begin]
                            : NULL,
        .dependency_count = row->dependency_count,
    };
    return true;
}

bool xr_module_task_graph_fingerprint(const XrModuleTaskGraph *graph,
                                      XrFingerprint *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!out || !graph_integrity(graph))
        return false;
    *out = graph->fingerprint;
    return true;
}

static void *worker_main(void *argument) {
    XrModuleTaskWorker *worker = (XrModuleTaskWorker *) argument;
    XrModuleTaskPool *pool = worker ? worker->pool : NULL;
    if (!pool)
        return NULL;
    for (uint32_t cursor = worker->ordinal; cursor < pool->ready_count;
         cursor += pool->worker_count) {
        uint32_t task = pool->ready[cursor];
        XrModuleTaskOutput output = {0};
        void *task_state = pool->task_state_size
                               ? pool->task_states +
                                     (size_t) task * pool->task_state_size
                               : NULL;
        bool succeeded =
            pool->execute(pool->graph, task, &output, task_state,
                          pool->context);
        output.diagnostic[sizeof(output.diagnostic) - 1u] = '\0';
        output.complete = true;
        output.succeeded = succeeded;
        pool->outputs[task] = output;
    }
    atomic_store_explicit(&worker->finished, true, memory_order_release);
    return NULL;
}

static uint32_t select_worker_count(uint32_t task_count, uint32_t limit) {
    uint32_t workers = limit ? limit : (uint32_t) xr_os_cpu_count();
    if (workers == 0)
        workers = 1;
    if (workers > XR_MODULE_TASK_MAX_WORKERS)
        workers = XR_MODULE_TASK_MAX_WORKERS;
    return workers < task_count ? workers : task_count;
}

bool xr_module_task_graph_run(const XrModuleTaskBatch *batch,
                              XrModuleTaskStats *stats, char *error,
                              size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (stats) {
        memset(stats, 0, sizeof(*stats));
        stats->first_failed_task = XR_MODULE_TASK_INDEX_NONE;
    }
    if (!batch || !stats || !batch->dependency_graph || !batch->task_graph ||
        !batch->execute || !batch->outputs || batch->output_count == 0) {
        if (error && error_size && !error[0])
            set_error(error, error_size,
                      "module task batch authority is invalid");
        return false;
    }
    uint32_t task_count = xr_module_task_graph_count(batch->task_graph);
    if (task_count == 0 || batch->output_count != task_count) {
        set_error(error, error_size,
                  "module task batch output authority is invalid");
        return false;
    }
    if ((batch->task_state_size == 0) != (batch->task_states == NULL) ||
        (batch->task_state_size != 0 &&
         task_count > SIZE_MAX / batch->task_state_size)) {
        set_error(error, error_size,
                  "module task batch state authority is invalid");
        return false;
    }
    memset(batch->outputs, 0,
           task_count * sizeof(*batch->outputs));
    if (batch->task_state_size)
        memset(batch->task_states, 0,
               (size_t) task_count * batch->task_state_size);
    if (!xr_module_task_graph_verify(batch->dependency_graph,
                                     batch->task_graph, error, error_size)) {
        if (error && error_size && !error[0])
            set_error(error, error_size,
                      "module task batch authority is invalid");
        return false;
    }

    uint32_t worker_capacity =
        select_worker_count(batch->task_graph->task_count,
                            batch->worker_limit);
    uint32_t *ready = (uint32_t *) xr_malloc(
        batch->task_graph->task_count * sizeof(*ready));
    XrModuleTaskWorker *workers = (XrModuleTaskWorker *) xr_calloc(
        worker_capacity, sizeof(*workers));
    xr_thread_t *threads = worker_capacity > 1u
                               ? (xr_thread_t *) xr_calloc(
                                     worker_capacity - 1u, sizeof(*threads))
                               : NULL;
    if (!ready || !workers || (worker_capacity > 1u && !threads)) {
        xr_free(threads);
        xr_free(workers);
        xr_free(ready);
        set_error(error, error_size,
                  "module task worker allocation failed");
        return false;
    }

    uint32_t max_level = 0;
    for (uint32_t task = 0; task < batch->task_graph->task_count; task++)
        if (batch->task_graph->tasks[task].level > max_level)
            max_level = batch->task_graph->tasks[task].level;

    bool ok = true;
    for (uint32_t level = 0; ok && level <= max_level; level++) {
        uint32_t ready_count = 0;
        for (uint32_t task = 0; task < batch->task_graph->task_count; task++)
            if (batch->task_graph->tasks[task].level == level)
                ready[ready_count++] = task;
        if (ready_count == 0)
            continue;
        uint32_t worker_count = worker_capacity < ready_count
                                    ? worker_capacity
                                    : ready_count;
        if (worker_count > stats->worker_count)
            stats->worker_count = worker_count;
        XrModuleTaskPool pool = {
            .graph = batch->task_graph,
            .ready = ready,
            .ready_count = ready_count,
            .worker_count = worker_count,
            .execute = batch->execute,
            .context = batch->context,
            .task_states = (uint8_t *) batch->task_states,
            .task_state_size = batch->task_state_size,
            .outputs = batch->outputs,
        };
        uint32_t created = 0;
        bool threads_ok = true;
        for (uint32_t worker = 0; worker < worker_count; worker++) {
            workers[worker].pool = &pool;
            workers[worker].ordinal = worker;
            atomic_init(&workers[worker].finished, false);
            if (worker != 0u &&
                !xr_thread_create(&threads[worker - 1u], worker_main,
                                  &workers[worker])) {
                threads_ok = false;
                break;
            }
            if (worker != 0u)
                created++;
        }
        if (threads_ok)
            (void) worker_main(&workers[0]);
        for (uint32_t thread = 0; thread < created; thread++) {
            if (xr_thread_join(threads[thread], NULL) != 0) {
                threads_ok = false;
                while (!atomic_load_explicit(&workers[thread + 1u].finished,
                                             memory_order_acquire))
                    xr_thread_yield();
                xr_thread_detach(threads[thread]);
            }
        }
        if (!threads_ok) {
            memset(batch->outputs, 0,
                   batch->output_count * sizeof(*batch->outputs));
            memset(stats, 0, sizeof(*stats));
            stats->first_failed_task = XR_MODULE_TASK_INDEX_NONE;
            set_error(error, error_size,
                      "module task worker lifecycle failed");
            ok = false;
            break;
        }
        stats->completed_count += ready_count;
        for (uint32_t cursor = 0; cursor < ready_count; cursor++) {
            uint32_t task = ready[cursor];
            if (!batch->outputs[task].complete ||
                !batch->outputs[task].succeeded) {
                stats->first_failed_task = task;
                set_error(error, error_size, "module task %u failed: %s",
                          task, batch->outputs[task].diagnostic[0]
                                    ? batch->outputs[task].diagnostic
                                    : "no diagnostic");
                ok = false;
                break;
            }
        }
    }
    xr_free(threads);
    xr_free(workers);
    xr_free(ready);
    return ok;
}

bool xr_module_task_graph_format_diagnostics(
    const XrModuleTaskGraph *graph, const XrModuleTaskOutput *outputs,
    uint32_t output_count, char *buffer, size_t buffer_size, size_t *written) {
    if (buffer && buffer_size)
        buffer[0] = '\0';
    if (written)
        *written = 0;
    if (!graph_integrity(graph) || !outputs ||
        output_count != graph->task_count || !buffer || buffer_size == 0 ||
        !written)
        return false;

    size_t cursor = 0;
    for (uint32_t task = 0; task < output_count; task++) {
        if (!outputs[task].complete || !outputs[task].diagnostic[0])
            continue;
        size_t length = 0;
        while (length < sizeof(outputs[task].diagnostic) &&
               outputs[task].diagnostic[length])
            length++;
        if (length == sizeof(outputs[task].diagnostic) ||
            length > SIZE_MAX - cursor - 2u ||
            cursor + length + 2u > buffer_size) {
            buffer[0] = '\0';
            return false;
        }
        memcpy(buffer + cursor, outputs[task].diagnostic, length);
        cursor += length;
        buffer[cursor++] = '\n';
        buffer[cursor] = '\0';
    }
    *written = cursor;
    return true;
}
