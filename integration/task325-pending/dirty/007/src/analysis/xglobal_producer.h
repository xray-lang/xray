/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_producer.h - Whole-program evidence producer from module ASTs
 */

#ifndef XGLOBAL_PRODUCER_H
#define XGLOBAL_PRODUCER_H

#include "xglobal_summary.h"

#include <stdbool.h>
#include <stdint.h>

struct XrModuleGraph;
struct XrModuleSpec;
struct XaAnalyzer;

/* Cache identity for evidence produced on behalf of a compiler executable.
 * An unavailable path falls back to the semantic-version identity, matching
 * the compiler's own fail-open behavior. */
XR_FUNC uint64_t xg_compiler_image_hash_for_path(const char *path);
XR_FUNC bool xg_module_summary_from_module_spec(XgModuleSummary *out_summary, XgModuleId module_id,
                                                const struct XrModuleSpec *spec);
XR_FUNC bool xg_standalone_build_key_from_module_spec(XgBuildKey *out_key,
                                                      const struct XrModuleSpec *spec,
                                                      uint32_t profile,
                                                      uint64_t imported_summary_hash);
XR_FUNC bool xg_build_key_from_ordered_module_specs(XgBuildKey *out_key,
                                                    const struct XrModuleSpec *const *specs,
                                                    uint32_t spec_count, uint32_t profile,
                                                    uint64_t imported_summary_hash);
XR_FUNC bool xg_build_key_from_module_graph(XgBuildKey *out_key, const struct XrModuleGraph *graph,
                                            uint32_t profile, uint64_t imported_summary_hash);
XR_FUNC bool xg_global_evidence_build_from_module_graph(XgGlobalEvidence *evidence,
                                                        const struct XrModuleGraph *graph,
                                                        uint32_t profile,
                                                        uint64_t imported_summary_hash);
XR_FUNC bool xg_global_evidence_build_from_module_graph_with_imported_modules(
    XgGlobalEvidence *evidence, const struct XrModuleGraph *graph, uint32_t profile,
    uint64_t imported_summary_hash, const XgModuleSummary *imported_modules,
    uint32_t imported_module_count);
XR_FUNC bool xg_global_evidence_build_from_module_graph_with_imported_modules_and_analyzer(
    XgGlobalEvidence *evidence, const struct XrModuleGraph *graph, uint32_t profile,
    uint64_t imported_summary_hash, const XgModuleSummary *imported_modules,
    uint32_t imported_module_count, struct XaAnalyzer *analyzer);
/* Canonical identity of the compile-time evidence module that owns the builtin
 * prelude enum declarations.  The producer publishes registry declarations
 * under it; consumers use it to tell a registry declaration apart from a
 * source enum with the same display name.  Returns 0 on allocation failure. */
XR_FUNC uint64_t xg_prelude_enum_module_canonical_hash(void);

#endif  // XGLOBAL_PRODUCER_H
