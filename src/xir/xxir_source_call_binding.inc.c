/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_call_binding.inc.c - Execute the HIR-owned argument permutation
 *
 * KEY CONCEPT:
 *   The source bridge reports checked binding failures and permutes completed
 *   values. Name matching and missing-parameter decisions belong to HIR.
 */
static bool source_call_binding_failure(SourceContext *ctx, AstNode *node,
    const SourceCallSyntax *call, uint32_t source, XrXirStatus status, XrHirCallBindingError error) {
    if (error.kind == XR_HIR_CALL_BIND_OK)
        return source_fail(ctx, node, status, "call binding has no declaration-owned parameter namespace");
    const char *message = NULL;
    switch (error.kind) {
    case XR_HIR_CALL_BIND_INDIRECT: message = "E0327: named arguments require a direct declaration call"; break;
    case XR_HIR_CALL_BIND_UNKNOWN: message = "E0327: unknown named argument"; break;
    case XR_HIR_CALL_BIND_DUPLICATE: message = "E0327: parameter is supplied more than once"; break;
    case XR_HIR_CALL_BIND_POSITIONAL_AFTER_NAMED: message = "E0327: positional argument follows a named argument"; break;
    case XR_HIR_CALL_BIND_NAMED_REST: message = "E0327: rest arguments must be positional"; break;
    case XR_HIR_CALL_BIND_NAMED_SPREAD: message = "E0327: a named argument supplies one value, not an argument spread"; break;
    case XR_HIR_CALL_BIND_TOO_MANY: message = "E0327: too many call arguments"; break;
    case XR_HIR_CALL_BIND_MISSING: message = "E0327: required parameter is not supplied"; break;
    default: return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "unknown call binding failure");
    }
    AstNode site = {0};
    if (node) { site.line = node->line; site.column = node->column; }
    if (source < (uint32_t)call->arg_count) {
        if (call->arguments[source]) {
            site.line = call->arguments[source]->line; site.column = call->arguments[source]->column;
        }
        if (call->arg_labels && call->arg_labels[source].span.line > 0) {
            site.line = call->arg_labels[source].span.line; site.column = call->arg_labels[source].span.column;
        }
    }
    return source_fail(ctx, &site, status, message);
}

static bool source_call_binding_begin(SourceContext *ctx, AstNode *node,
    SourceCallPlan *plan, const SourceCallSyntax *call) {
    if (!call->arg_labels) return true;
    XrHirCallBindingError error = {0};
    XrXirStatus status = xr_hir_body_call_bind_open(ctx->typed_body, plan->signature, ctx->request, &error);
    if (status != XR_XIR_OK) {
        uint32_t first = 0;
        while (first < (uint32_t)call->arg_count && !call->arg_labels[first].name) ++first;
        return source_call_binding_failure(ctx, node, call, first, status, error);
    }
    plan->named_arguments = true;
    return true;
}

static bool source_call_binding_item(SourceContext *ctx, AstNode *node, SourceCallPlan *plan,
    const SourceCallSyntax *call, uint32_t source, uint32_t arity, bool spread, uint32_t *formal) {
    if (!plan->named_arguments) return true;
    const char *name = call->arg_labels[source].name;
    size_t length = name ? source_text_size(ctx, name) : 0;
    if (length > UINT32_MAX) return source_fail(ctx, node, XR_XIR_OVERFLOW, "argument label is too long");
    XrHirCallBindingError error = {0};
    XrXirStatus status = xr_hir_body_call_bind_argument(ctx->typed_body, plan->signature,
        name, (uint32_t)length, arity, spread, ctx->request, formal, &error);
    return status == XR_XIR_OK || source_call_binding_failure(ctx, node, call, source, status, error);
}

static bool source_call_binding_complete(SourceContext *ctx, AstNode *node,
    SourceCallPlan *plan, const SourceCallSyntax *call, uint32_t count) {
    if (!plan->named_arguments) return true;
    XrHirCallBindingError error = {0};
    XrXirStatus status = xr_hir_body_call_bind_seal(ctx->typed_body, plan->signature,
        ctx->request, &plan->binding, &error);
    if (status != XR_XIR_OK) return source_call_binding_failure(ctx, node, call, UINT32_MAX, status, error);
    if (count > UINT32_MAX - plan->value_offset)
        return source_fail(ctx, node, XR_XIR_OVERFLOW, "argument storage count overflow");
    plan->ordered_values = plan->values;
    plan->values = source_recipe_storage(ctx, count + plan->value_offset, sizeof(*plan->values));
    return plan->values != NULL;
}

static bool source_call_binding_values(SourceContext *ctx, AstNode *node, const SourceCallPlan *plan) {
    if (!plan->named_arguments) return true;
    if (!plan->ordered_values || !plan->binding.argument_formals || !plan->values)
        return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "call binding lost its argument storage");
    for (uint32_t a = 0; a < plan->binding.argument_count; ++a) {
        if (!(a % 64) && !source_request_poll(ctx, node)) return false;
        uint32_t p = plan->binding.argument_formals[a];
        if (plan->rest_count && p + 1 == plan->rest_count) continue;
        if (p < plan->parameter_offset || p >= plan->binding.parameter_count)
            return source_fail(ctx, node, XR_XIR_BAD_STRUCTURE, "call binding formal is out of range");
        plan->ordered_values[p - plan->parameter_offset + plan->value_offset] = plan->values[a + plan->value_offset];
    }
    return true;
}

static bool source_call_default_needed(const SourceCallPlan *plan, uint32_t formal, uint32_t supplied) {
    if (formal < plan->parameter_offset) return false;
    return plan->named_arguments ? plan->binding.formal_arguments[formal] == UINT32_MAX : formal >= supplied;
}

static uint32_t source_call_final_argument(const SourceCallPlan *plan, uint32_t formal) {
    return plan->named_arguments ? plan->binding.formal_arguments[formal] : formal - plan->parameter_offset;
}
