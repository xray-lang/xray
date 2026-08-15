/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_module_task_graph.h - Deterministic module SCC task scheduling
 *
 * KEY CONCEPT:
 *   Each task is one strongly connected module component. Dependency tasks
 *   precede their consumers, while independent tasks in one level may run in
 *   parallel and publish only canonical task-indexed result rows.
 */

#ifndef XR_MODULE_TASK_GRAPH_H
#define XR_MODULE_TASK_GRAPH_H

#include "xr_dependency_graph.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XR_MODULE_TASK_DIAGNOSTIC_CAPACITY 256u
#define XR_MODULE_TASK_INDEX_NONE UINT32_MAX

typedef struct XrModuleTaskGraph XrModuleTaskGraph;

typedef struct XrModuleTaskView {
    uint32_t level;
    const XrStableId *members;
    uint32_t member_count;
    const uint32_t *dependencies;
    uint32_t dependency_count;
} XrModuleTaskView;

typedef struct XrModuleTaskOutput {
    XrFingerprint artifact_fingerprint;
    char diagnostic[XR_MODULE_TASK_DIAGNOSTIC_CAPACITY];
    bool complete;
    bool succeeded;
} XrModuleTaskOutput;

typedef bool (*XrModuleTaskExecuteFn)(const XrModuleTaskGraph *graph,
                                      uint32_t task_index,
                                      XrModuleTaskOutput *output,
                                      void *context);

typedef struct XrModuleTaskBatch {
    const XrDependencyGraph *dependency_graph;
    const XrModuleTaskGraph *task_graph;
    uint32_t worker_limit;
    XrModuleTaskExecuteFn execute;
    void *context;
    XrModuleTaskOutput *outputs;
    uint32_t output_count;
} XrModuleTaskBatch;

typedef struct XrModuleTaskStats {
    uint32_t worker_count;
    uint32_t completed_count;
    uint32_t first_failed_task;
} XrModuleTaskStats;

XR_FUNC bool xr_module_task_graph_build(const XrDependencyGraph *dependencies,
                                        XrModuleTaskGraph **out, char *error,
                                        size_t error_size);
XR_FUNC void xr_module_task_graph_free(XrModuleTaskGraph *graph);
XR_FUNC bool xr_module_task_graph_verify(const XrDependencyGraph *dependencies,
                                         const XrModuleTaskGraph *graph,
                                         char *error, size_t error_size);
XR_FUNC uint32_t xr_module_task_graph_count(const XrModuleTaskGraph *graph);
XR_FUNC bool xr_module_task_graph_task(const XrModuleTaskGraph *graph,
                                       uint32_t task_index,
                                       XrModuleTaskView *out);
XR_FUNC bool xr_module_task_graph_fingerprint(const XrModuleTaskGraph *graph,
                                              XrFingerprint *out);

/* Execute callbacks may write only their provided output row. Results and
 * diagnostics become observable after all tasks in the current level join. */
XR_FUNC bool xr_module_task_graph_run(const XrModuleTaskBatch *batch,
                                      XrModuleTaskStats *stats, char *error,
                                      size_t error_size);
XR_FUNC bool xr_module_task_graph_format_diagnostics(
    const XrModuleTaskGraph *graph, const XrModuleTaskOutput *outputs,
    uint32_t output_count, char *buffer, size_t buffer_size,
    size_t *written);

#endif  // XR_MODULE_TASK_GRAPH_H
