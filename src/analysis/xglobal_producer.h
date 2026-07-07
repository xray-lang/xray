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

XR_FUNC bool xg_global_evidence_build_from_module_graph(XgGlobalEvidence *evidence,
                                                        const struct XrModuleGraph *graph,
                                                        uint32_t profile);

#endif  // XGLOBAL_PRODUCER_H
