/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_closure_parameters.inc.c - Callable modes on authentic parameters
 *
 * KEY CONCEPT:
 *   A ref parameter names the caller's Cell while queries expose its element.
 *   Contextual modes do not invent source tokens or capture authority.
 */
static bool closure_parameters(SourceContext *ctx, AstNode *node, const SourceCaptureScan *scan,
    XrXirCallableParameter *parameters, const XrXirTypeNode *context) {
    if (!source_capture_parameters(ctx, node, scan)) return false;
    uint32_t scope=0;
    if(!xr_hir_items_find_scope(ctx->items,ctx->module,node->node_id,&scope))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"closure parameter scope was not reserved");
    SourceFunction *body = &ctx->bodies[ctx->function];
    FunctionDeclNode *decl = &node->as.function_expr;
    for (int i = 0; i < decl->param_count; ++i) {
        XrParamNode *param = decl->params[i];
        XrXirType type;
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if ((param->passing_mode != XR_PARAM_READ && param->passing_mode != XR_PARAM_REF) ||
            param->default_value || param->pattern || param->is_rest || (!param->type && !context))
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure parameter requires an annotation or complete callable context");
        XrParamMode mode = param->passing_mode;
        if (context && mode == XR_PARAM_READ) mode = (XrParamMode)context->parameters[i].mode;
        if (context && (uint32_t)mode != context->parameters[i].mode)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure parameter mode differs from its callable context");
        bool by_reference = mode == XR_PARAM_REF;
        if (param->type) {
            if (!source_type(ctx, param->type, &type)) return false;
        } else {
            type = context->parameters[i].type;
            if (by_reference) type = xr_xir_cell_element(&ctx->types, type);
        }
        if (type == XR_XIR_UNIT && !by_reference)
            return source_fail(ctx, node, XR_XIR_BAD_TYPE, "closure parameter cannot be unit");
        SourceName *symbol = source_parameter_name(ctx,node,param,scope+1);
        if (!symbol) return false;
        symbol->kind = SOURCE_LOCAL; symbol->index = scan->count + (uint32_t)i;
        symbol->type=type;symbol->mutable=by_reference;symbol->declared_mode=mode;
        XrXirSourceRange range = {ctx->module, param->line, param->column, param->line, 0};
        if (param->column > 0 && source_text_size(ctx, param->name) <= (size_t)(INT_MAX - param->column))
            range.end_column = param->column + (int)source_text_size(ctx, param->name);
        if (!source_query_declare(ctx, symbol, XR_XIR_SOURCE_PARAMETER, body->declaration, range) ||
            !source_query_mode(ctx, symbol->declaration, param->passing_mode, param->mode_span)) return false;
        if(!source_hir_parameter_declared(ctx,symbol))return false;
        source_query_binding_type(ctx, symbol);
        if (by_reference && !source_cell_type(ctx, type, &type)) return false;
        body->parameters[symbol->index] = type;
        parameters[i] = (XrXirCallableParameter){type, mode};
    }
    return true;
}
