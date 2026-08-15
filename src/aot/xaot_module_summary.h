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
 *   After every module owns a verified SemanticPlan, the build derives one
 *   module summary per module, publishes the resulting dependency graph into
 *   the compiler session, and round-trips each module's XSM artifact through
 *   the incremental cache. A cache hit is evidence that the module's verified
 *   semantics are unchanged; it never replaces a proof this build already owes.
 */

#ifndef XAOT_MODULE_SUMMARY_H
#define XAOT_MODULE_SUMMARY_H

#include "xaot_driver.h"

struct XiModule;
struct XrCompilerSession;
struct XrModuleGraph;

/* Modules are indexed by topological position, matching graph->topo_order.
 * Diagnostics are written to stderr and the verbose report to stdout, so the
 * caller keeps a single failure branch. */
XR_FUNC bool xaot_publish_module_summaries(struct XrCompilerSession *session,
                                           const struct XrModuleGraph *graph,
                                           struct XiModule *const *modules, int module_count,
                                           const XaotBuildOptions *options, bool verbose,
                                           XaotModuleSummaryCacheStats *stats);

#endif  // XAOT_MODULE_SUMMARY_H
