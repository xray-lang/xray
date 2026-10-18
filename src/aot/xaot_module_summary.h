/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_module_summary.h - Per-module summary publication for a native build
 *
 * KEY CONCEPT:
 *   A build owns its dependency graph, task rows and cache borrows. Preparation
 *   may run in parallel; every prepared row is preflighted before canonical
 *   publication. A cache hit never replaces the semantic proof owed by this build.
 */

#ifndef XAOT_MODULE_SUMMARY_H
#define XAOT_MODULE_SUMMARY_H

#include "xaot_driver.h"

struct XiModule;
struct XrCacheStore;
struct XrModuleGraph;
struct XrProgramSemanticClosure;

/* All pointers are borrowed through this synchronous operation. Modules follow
 * graph->topo_order. The graph's actual compiler state owns task-row storage;
 * a cache store, when supplied, must use that same resource ledger. */
typedef struct XaotModuleSummaryInput {
    struct XrCacheStore *cache_store;
    const struct XrModuleGraph *graph;
    struct XiModule *const *modules;
    int module_count;
    const struct XrProgramSemanticClosure *program_closure;
    const XaotBuildOptions *options;
    bool verbose;
} XaotModuleSummaryInput;

XR_FUNC bool xaot_publish_module_summaries(const XaotModuleSummaryInput *input,
    XaotModuleSummaryCacheStats *stats);

#endif  // XAOT_MODULE_SUMMARY_H
