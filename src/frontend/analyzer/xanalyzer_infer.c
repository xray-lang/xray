/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_infer.c - Type inference implementation
 */

#include "xanalyzer_infer.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include <string.h>

// Create inference context
XaInferContext *xa_infer_context_new(XaAnalyzer *analyzer) {
    XR_DCHECK(analyzer != NULL, "infer_context_new: NULL analyzer");
    XaInferContext *ctx = xr_calloc(1, sizeof(XaInferContext));
    if (!ctx) return NULL;

    ctx->analyzer = analyzer;
    ctx->flow = xa_flow_builder_new();
    ctx->cache = xa_flow_cache_new();

    // Initialize flow graph with start node (critical for type narrowing)
    if (ctx->flow) {
        xa_flow_create_start(ctx->flow);
    }

    return ctx;
}

// Free inference context
void xa_infer_context_free(XaInferContext *ctx) {
    if (!ctx) return;

    if (ctx->flow) xa_flow_builder_free(ctx->flow);
    if (ctx->cache) xa_flow_cache_free(ctx->cache);
    if (ctx->return_types) xr_free(ctx->return_types);

    xr_free(ctx);
}

// Add return type (for function return type inference)
void xa_infer_add_return_type(XaInferContext *ctx, XrType *type) {
    if (!ctx || !type) return;

    if (ctx->return_type_count >= ctx->return_type_capacity) {
        int new_cap = ctx->return_type_capacity == 0 ? 8 : ctx->return_type_capacity * 2;
        XR_REALLOC_OR_ABORT(ctx->return_types,
                            sizeof(XrType*) * (size_t)new_cap,
                            "infer return_types grow");
        ctx->return_type_capacity = new_cap;
    }
    ctx->return_types[ctx->return_type_count++] = type;
}

// Compute unified return type from all return statements
XrType *xa_infer_compute_return_type(XaInferContext *ctx) {
    if (!ctx || ctx->return_type_count == 0) {
        return xr_type_new_void();
    }

    if (ctx->return_type_count == 1) {
        return ctx->return_types[0];
    }

    // Create union of all return types
    XrType *result = ctx->return_types[0];
    for (int i = 1; i < ctx->return_type_count; i++) {
        result = xr_type_union(result, ctx->return_types[i]);
    }

    return result;
}

