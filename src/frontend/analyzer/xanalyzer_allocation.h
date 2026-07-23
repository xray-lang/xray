/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_allocation.h - Canonical allocation-effect inference
 */

#ifndef XANALYZER_ALLOCATION_H
#define XANALYZER_ALLOCATION_H

#include "../../base/xdefs.h"

struct XaAnalyzer;
struct AstNode;

/* Pass 4: infer allocation effects for every function/method. External verify
 * contracts consume these facts. The pass runs before either backend sees the program. */
XR_FUNC void xa_infer_allocation_effects(struct XaAnalyzer *analyzer, struct AstNode *ast);

#endif /* XANALYZER_ALLOCATION_H */
