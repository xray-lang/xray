/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_memory_effect.h - Root-relative memory-effect inference
 */

#ifndef XANALYZER_MEMORY_EFFECT_H
#define XANALYZER_MEMORY_EFFECT_H

#include "../../base/xdefs.h"

struct AstNode;
struct XaAnalyzer;

/* Runs after type inference and parameter-mutation propagation.  Publishes a
 * canonical summary for every function-like symbol. */
XR_FUNC void xa_infer_memory_effects(struct XaAnalyzer *analyzer, struct AstNode *ast);

#endif /* XANALYZER_MEMORY_EFFECT_H */
