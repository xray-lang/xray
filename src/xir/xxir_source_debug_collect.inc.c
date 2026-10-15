/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_debug_collect.inc.c - Publish the checked recipe correspondence
 */
static XrXirStatus source_debug_publish(SourceContext *ctx,XrXirSourceResult *output) {
    const XrXirModule *module=xr_xir_compile_artifact_module(output->checked);
    if (!module || module->function_count!=ctx->function_count) return XR_XIR_BAD_STRUCTURE;
    XrXirSourceDebugFunction *functions=source_alloc(ctx,ctx->function_count,sizeof(*functions));
    if (ctx->function_count && !functions) return ctx->diagnostic.status;
    for (uint32_t f=0;f<ctx->function_count;++f) {
        if (!source_work_units(ctx,NULL,sizeof(*functions))) return ctx->diagnostic.status;
        functions[f]=(XrXirSourceDebugFunction){f,ctx->bodies[f].declaration,
            module->functions[f].parameter_count,module->functions[f].instruction_count,ctx->bodies[f].debug_points,
            ctx->bodies[f].debug_bindings,ctx->bodies[f].debug_binding_count};
    }
    XrXirSourceDebugView debug={functions,ctx->function_count};
    return xr_xir_compile_source_snapshot_debug_copy(output->snapshot,&debug,module);
}
