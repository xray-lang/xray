/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Source submits complete declaration spellings and consumes HIR diagnostics. */
static bool source_escape_prepare(SourceContext *ctx) {
    XrHirEscapeDeclaration *declarations = source_alloc(ctx, ctx->function_count, sizeof(*declarations));
    if (ctx->function_count && !declarations) return false;
    for (uint32_t f = 0; f < ctx->function_count; ++f) {
        if (!source_request_poll(ctx, NULL)) return false;
        AstNode *node = ctx->bodies[f].node;
        XrParamNode *const *parameters = NULL;
        uint32_t count = 0;
        if (node && (node->type == AST_FUNCTION_DECL || node->type == AST_FUNCTION_EXPR)) {
            parameters = node->as.function_decl.params;
            count = (uint32_t)node->as.function_decl.param_count;
        } else if (node && node->type == AST_METHOD_DECL) {
            parameters = node->as.method_decl.params;
            count = (uint32_t)node->as.method_decl.param_count;
        }
        uint32_t physical = ctx->functions[f].parameter_count;
        if (count > physical) continue;
        const char **names = count ? source_alloc(ctx, count, sizeof(*names)) : NULL;
        if (count && !names) return false;
        for (uint32_t p = 0; p < count; ++p) names[p] = parameters[p]->name;
        declarations[f] = (XrHirEscapeDeclaration){names, count, physical - count};
    }
    XrXirDeclarations identities;
    XrXirModule module = source_module_view(ctx, &identities);
    XrHirEscapeDiagnostic diagnostic = {0};
    XrXirStatus status = xr_hir_escape_check(&ctx->compile, &module, declarations, ctx->request, &diagnostic);
    if (status == XR_XIR_OK) return true;
    AstNode *site = NULL;
    if (diagnostic.function < ctx->function_count) {
        SourceFunction *body = &ctx->bodies[diagnostic.function];
        site = body->node;
        ctx->module = body->module;
    }
    source_fail(ctx, site, status, diagnostic.message[0] ? diagnostic.message : "callable lifetime checking failed");
    if (diagnostic.function < ctx->function_count &&
        diagnostic.instruction < ctx->functions[diagnostic.function].instruction_count &&
        ctx->bodies[diagnostic.function].debug_points) {
        XrXirSourceRange range = ctx->bodies[diagnostic.function].debug_points[diagnostic.instruction].range;
        ctx->diagnostic.module = range.module;
        ctx->diagnostic.line = range.line;
        ctx->diagnostic.column = range.column;
    }
    if (diagnostic.code && ctx->graph && ctx->diagnostic.module < (uint32_t)ctx->graph->spec_count) {
        const XrModuleSpec *source = &ctx->graph->specs[ctx->diagnostic.module];
        uint32_t text = 0;
        XrCompileResourceStatus recorded = xr_frontend_report_new(ctx->compile.resources, &ctx->report);
        if (recorded == XR_COMPILE_RESOURCE_OK)
            recorded = xr_frontend_report_source(ctx->report, source->source_path,
                source->source_text, source->source_length, NULL, &text);
        if (recorded == XR_COMPILE_RESOURCE_OK) {
            XrFrontendLocation location = xr_frontend_report_location(ctx->report, text,
                ctx->diagnostic.line, ctx->diagnostic.column, NULL);
            recorded = xr_frontend_report_add(ctx->report, diagnostic.code, diagnostic.message, location, NULL, 0);
        }
        if (recorded != XR_COMPILE_RESOURCE_OK) ctx->diagnostic.status = xir_compile_resource_status(recorded);
        else xr_frontend_report_seal(ctx->report, false);
    }
    return false;
}
