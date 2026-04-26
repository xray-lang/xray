/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_infer.h - Type inference for expressions and statements
 *
 * KEY CONCEPT:
 *   Multi-pass analysis:
 *   1. Symbol collection - gather all declarations
 *   2. Type inference - compute types bottom-up
 *   3. Type checking - verify type compatibility
 */

#ifndef XANALYZER_INFER_H
#define XANALYZER_INFER_H

#include "xanalyzer.h"
#include "xanalyzer_flow.h"
#include "../../base/xdefs.h"

// Inference context (for a single file/function)
typedef struct XaInferContext {
    XaAnalyzer *analyzer;
    XaFlowBuilder *flow;
    XaFlowCache *cache;

    // Current function being analyzed
    XaSymbol *current_function;
    XrType *expected_return_type;

    // Collected return types (for inference)
    XrType **return_types;
    int return_type_count;
    int return_type_capacity;

    // File info
    const char *file_path;

    // Expected type for bidirectional inference (contextual typing)
    // Propagated from declaration to initializer expression
    XrType *expected_type;

    // Generic type inference context (for callback parameters)
    // e.g., arr.map(x => x+1) - element_type is int, so x: int
    XrType *callback_element_type;      // Element type for callback first param
    XrType *callback_index_type;        // Index type for callback second param (always int)
    XrType *callback_accumulator_type;  // Accumulator type for reduce (from initial value)
    XrType *callback_array_type;        // Original array type for callbacks needing it
} XaInferContext;

// API: Context lifecycle
XR_FUNC XaInferContext *xa_infer_context_new(XaAnalyzer *analyzer);
XR_FUNC void xa_infer_context_free(XaInferContext *ctx);

// Return type inference
XR_FUNC void xa_infer_add_return_type(XaInferContext *ctx, XrType *type);
XR_FUNC XrType *xa_infer_compute_return_type(XaInferContext *ctx);

#endif  // XANALYZER_INFER_H
