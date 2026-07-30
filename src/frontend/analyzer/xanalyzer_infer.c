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
#include <stdio.h>
#include <string.h>

// Create inference context
XaInferContext *xa_infer_context_new(XaAnalyzer *analyzer) {
    XR_DCHECK(analyzer != NULL, "infer_context_new: NULL analyzer");
    XaInferContext *ctx = xr_calloc(1, sizeof(XaInferContext));
    if (!ctx)
        return NULL;

    ctx->analyzer = analyzer;
    ctx->flow = xa_flow_builder_new();
    ctx->cache = xa_flow_cache_new();
    ctx->current_block_stmt_index = -1;

    // Initialize flow graph with start node (critical for type narrowing)
    if (ctx->flow) {
        xa_flow_create_start(ctx->flow);
    }

    return ctx;
}

// Free inference context
void xa_infer_context_free(XaInferContext *ctx) {
    if (!ctx)
        return;

    if (ctx->flow)
        xa_flow_builder_free(ctx->flow);
    if (ctx->cache)
        xa_flow_cache_free(ctx->cache);
    if (ctx->return_types)
        xr_free(ctx->return_types);
    while (ctx->infer_vars) {
        XaInferVar *next = ctx->infer_vars->next;
        xr_free(ctx->infer_vars->lower_bounds);
        xr_free(ctx->infer_vars->upper_bounds);
        xr_free(ctx->infer_vars->constraints);
        xr_free(ctx->infer_vars);
        ctx->infer_vars = next;
    }
    while (ctx->active_loans) {
        XaActiveLoan *next = ctx->active_loans->next;
        xr_free(ctx->active_loans->owner_path);
        xr_free(ctx->active_loans);
        ctx->active_loans = next;
    }

    xr_free(ctx);
}

XaInferVar *xa_infer_var_new(XaInferContext *ctx, const char *reason, const XrLocation *loc) {
    if (!ctx)
        return NULL;

    XaInferVar *var = xr_calloc(1, sizeof(XaInferVar));
    if (!var)
        return NULL;

    var->id = ++ctx->next_infer_var_id;
    var->reason = reason;
    if (loc)
        var->loc = *loc;
    var->next = ctx->infer_vars;
    ctx->infer_vars = var;
    ctx->infer_var_count++;
    return var;
}

XrType *xa_infer_var_report_unsolved(XaInferContext *ctx, XaInferVar *var, const char *message) {
    if (!ctx || !ctx->analyzer)
        return xr_type_new_error(NULL);

    if (var && !var->reported_unsolved) {
        const char *base = message ? message : "cannot infer type";
        char msg[256];
        snprintf(msg, sizeof(msg), "%s", base);
        xa_analyzer_add_diagnostic(ctx->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE_TYPE_MISMATCH,
                                   msg, var->loc.file ? &var->loc : NULL);
        var->reported_unsolved = true;
        ctx->unresolved_infer_var_count++;
        ctx->analyzer->unresolved_inference_count++;
    }

    return xr_type_new_error(ctx->analyzer->isolate);
}

// Add return type (for function return type inference)
void xa_infer_add_return_type(XaInferContext *ctx, XrType *type) {
    if (!ctx || !type)
        return;

    if (ctx->return_type_count >= ctx->return_type_capacity) {
        int new_cap = ctx->return_type_capacity == 0 ? 8 : ctx->return_type_capacity * 2;
        XR_REALLOC_OR_ABORT(ctx->return_types, sizeof(XrType *) * (size_t) new_cap,
                            "infer return_types grow");
        ctx->return_type_capacity = new_cap;
    }
    ctx->return_types[ctx->return_type_count++] = type;
}

// Compute unified return type from all return statements
XrType *xa_infer_compute_return_type(XaInferContext *ctx) {
    if (!ctx || ctx->return_type_count == 0) {
        return xr_type_new_unit(NULL);
    }

    if (ctx->return_type_count == 1) {
        return ctx->return_types[0];
    }

    // Create union of all return types
    XrType *result = ctx->return_types[0];
    for (int i = 1; i < ctx->return_type_count; i++) {
        result = xr_type_union(ctx->analyzer->isolate, result, ctx->return_types[i]);
    }

    return result;
}
