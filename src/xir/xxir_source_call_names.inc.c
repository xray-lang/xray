/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_names.inc.c - Authentic declaration parameter-name snapshots
 *
 * KEY CONCEPT:
 *   Signature identities select declarations. Call-site labels never select or
 *   manufacture the parameter namespace passed to the HIR owner.
 */
static XrXirStatus source_hir_call_names(void *data, const XrHirSignatureInput *signature,
                                      XrHirCallDeclarationNames *output) {
    SourceContext *ctx = data;
    if (ctx->diagnostic.status != XR_XIR_OK) return ctx->diagnostic.status;
    AstNode *node = NULL;
    XrXirLiteral *cached = NULL;
    bool requirement = signature->family == XR_HIR_CALL_REQUIREMENT;
    if (requirement) {
        uint32_t owner = signature->interface_declaration, member = signature->interface_member;
        if (owner >= ctx->interfaces.count || !ctx->interface_sources || !ctx->interface_sources[owner])
            return XR_XIR_BAD_STRUCTURE;
        if (member>=ctx->interfaces.declarations[owner].method_count) return XR_XIR_BAD_STRUCTURE;
        SourceImportedParameterNames *entries=owner<ctx->imported_parameter_owner_count?
            ctx->imported_parameter_names[owner]:NULL;
        if (entries && entries[member].present) {
            const SourceImportedParameterNames *entry=&entries[member];
            if (signature->parameter_offset || entry->row.count!=signature->count) return XR_XIR_BAD_STRUCTURE;
            *output=(XrHirCallDeclarationNames){entry->row.names,entry->row.count};return XR_XIR_OK;
        }
        node = ctx->interface_sources[owner]->node;
        if (!node || node->type != AST_INTERFACE_DECL || member >= (uint32_t)node->as.interface_decl.method_count)
            return XR_XIR_BAD_STRUCTURE;
        node = node->as.interface_decl.methods[member];
    } else if (signature->family == XR_HIR_CALL_FUNCTION || signature->family == XR_HIR_CALL_CONSTRUCTOR) {
        if (signature->function >= ctx->function_count) return XR_XIR_BAD_STRUCTURE;
        SourceFunction declaration = ctx->bodies[signature->function];
        node = declaration.node;
        cached = declaration.call_parameter_names;
        if (declaration.call_parameter_namespace) {
            *output=(XrHirCallDeclarationNames){cached,signature->count};return XR_XIR_OK;
        }
        if (declaration.checked_library) return XR_XIR_BAD_TYPE;
    } else return XR_XIR_BAD_TYPE;
    if (cached) {
        *output = (XrHirCallDeclarationNames){cached, signature->count};
        return XR_XIR_OK;
    }
    XrParamNode **parameters = NULL;
    int count = 0;
    if (node && node->type == AST_METHOD_DECL) {
        parameters = node->as.method_decl.params; count = node->as.method_decl.param_count;
    } else if (node && node->type == AST_FUNCTION_DECL) {
        parameters = node->as.function_decl.params; count = node->as.function_decl.param_count;
    } else if (node && node->type == AST_INTERFACE_METHOD) {
        parameters = node->as.interface_method.params; count = node->as.interface_method.param_count;
    } else if (node && node->type == AST_FUNCTION_EXPR) {
        return XR_XIR_BAD_TYPE;
    } else if (signature->count != signature->parameter_offset) {
        return XR_XIR_BAD_STRUCTURE;
    }
    if (count < 0 || (count && !parameters) || signature->parameter_offset > signature->count ||
        (uint32_t)count != signature->count - signature->parameter_offset) return XR_XIR_BAD_STRUCTURE;
    XrXirLiteral *names = signature->count ? source_alloc(ctx, signature->count, sizeof(*names)) : NULL;
    if (signature->count && !names) return ctx->diagnostic.status;
    for (uint32_t p = 0; p < signature->parameter_offset; ++p) names[p] = (XrXirLiteral){0};
    for (int p = 0; p < count; ++p) {
        if (!source_request_poll(ctx, node)) return ctx->diagnostic.status;
        const XrParamNode *parameter = parameters[p];
        if (!parameter) return XR_XIR_BAD_STRUCTURE;
        if (parameter->pattern) {
            names[(uint32_t)p + signature->parameter_offset] = (XrXirLiteral){0};
            continue;
        }
        if (!parameter->name || !*parameter->name) return XR_XIR_BAD_STRUCTURE;
        const char *name = source_owned_text(ctx, parameter->name);
        if (!name) return ctx->diagnostic.status;
        size_t length = source_text_size(ctx, name);
        if (length > UINT32_MAX) return XR_XIR_OVERFLOW;
        names[(uint32_t)p + signature->parameter_offset] = (XrXirLiteral){name, (uint32_t)length};
    }
    if (!requirement) {
        ctx->bodies[signature->function].call_parameter_names = names;
        ctx->bodies[signature->function].call_parameter_namespace = true;
    }
    *output = (XrHirCallDeclarationNames){names, signature->count};
    return XR_XIR_OK;
}
