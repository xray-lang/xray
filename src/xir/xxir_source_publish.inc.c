/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_publish.inc.c - Publish independently owned query observations
 */
static void source_query_publish(SourceContext *ctx, XrXirSourceResult *output) {
    if (!output || !ctx->query_ready || ctx->diagnostic.status != XR_XIR_OK) {
        if (output) xr_xir_compile_source_result_free(output);
        return;
    }
    source_release_retired_arrays(ctx);
    ctx->query.complete = ctx->diagnostic.status == XR_XIR_OK;
    ctx->query.implementations = ctx->implementations_ready && ctx->implementations.count ? &ctx->implementations : NULL;
    ctx->query.diagnostic = ctx->diagnostic;
    ctx->query.types = ctx->types.count || ctx->types.nominals || ctx->types.interfaces ? &ctx->types : NULL;
    if(!source_native_query_types(ctx) || !source_hash_query_types(ctx)){xr_xir_compile_source_result_free(output);return;}
    if(!source_display_names_publish(ctx)){xr_xir_compile_source_result_free(output);return;}
    const char *failure = "source query snapshot publication failed";
    XrXirStatus status = xr_xir_compile_source_snapshot_copy_v2(&ctx->compile, &ctx->query, xr_xir_compile_artifact_construction(output->checked), &output->snapshot);
    if (status == XR_XIR_OK) {
        failure = "source syntax snapshot publication failed";
        status = xr_xir_compile_source_snapshot_syntax_copy(output->snapshot, &ctx->syntax);
    }
    if (status == XR_XIR_OK) {
        failure = "source call snapshot publication failed";
        status = source_calls_publish(ctx, output->snapshot);
    }
    if (status == XR_XIR_OK) {
        failure = "source debug snapshot publication failed";
        status = source_debug_publish(ctx, output);
    }
    if (status == XR_XIR_OK) {
        failure = "source dependency snapshot publication failed";
        status = source_dependencies_publish(ctx, output->snapshot);
    }
    if (status == XR_XIR_OK) {
        failure = "source type snapshot publication failed";
        status = source_type_facts_publish(ctx, output->snapshot);
    }
    if (status==XR_XIR_OK && ctx->semantic_output && !source_semantic_capture(ctx,ctx->semantic_output)) {
        xr_xir_compile_source_result_free(output);return;
    }
    if (status != XR_XIR_OK) {
        xr_xir_compile_source_result_free(output); memset(&ctx->diagnostic, 0, sizeof(ctx->diagnostic));
        source_fail(ctx, NULL, status, failure);
    }
}
