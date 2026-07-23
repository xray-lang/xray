/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_suspend.h - Canonical suspend-effect inference
 */

#ifndef XANALYZER_SUSPEND_H
#define XANALYZER_SUSPEND_H

#include "../../base/xdefs.h"

struct XaAnalyzer;
struct AstNode;

/* Pass 5: infer the suspend effect (task 212 semantics) for every
 * function/method and validate typed contracts plus manifest-owned entry and
 * synchronous export boundaries. The pass is analyzer-owned
 * and fail-closed: a body that may suspend, or whose suspend evidence is
 * incomplete (dynamic call target, open dispatch), fails the contract. */
XR_FUNC void xa_verify_no_suspend(struct XaAnalyzer *analyzer, struct AstNode *ast);

#endif /* XANALYZER_SUSPEND_H */
