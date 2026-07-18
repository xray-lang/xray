/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_allocation.h - Allocation effect inference and @no_alloc validation
 */

#ifndef XANALYZER_ALLOCATION_H
#define XANALYZER_ALLOCATION_H

#include "../../base/xdefs.h"

struct XaAnalyzer;
struct AstNode;

/* Pass 4: infer allocation effects for every function/method and validate
 * @no_alloc assertions. The pass is analyzer-owned and runs for check, VM and
 * AOT frontends before either backend sees the program. */
XR_FUNC void xa_infer_allocation_effects(struct XaAnalyzer *analyzer, struct AstNode *ast);

#endif /* XANALYZER_ALLOCATION_H */
