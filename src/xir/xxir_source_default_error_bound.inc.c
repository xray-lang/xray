/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_default_error_bound.inc.c - Empty error bounds for constant defaults
 *
 * KEY CONCEPT:
 *   Refine only this producer's real default helpers before their heads freeze.
 *   Calls and all other recipes retain the explicit conservative bound.
 */
static bool source_constant_default_error_bound(SourceContext *ctx) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    XrXirFunction *function=&ctx->functions[ctx->function];
    if (body->checked_library || function->error_type!=XR_XIR_ERROR) return true;
    const XrXirFunctionIdentity *identity=&ctx->identities[ctx->function];
    bool field=body->node && body->node->type==AST_FIELD_DECL &&
        identity->nominal_owner && identity->method_kind==XR_XIR_MEMBER_HELPER;
    /* Only parameter-default construction sets default_expression. An ordinary
     * declaration, module initializer or implicit constructor is not this role. */
    if ((!body->default_expression && !field) || !body->count) return true;
    for (uint32_t i=0;i<body->count;++i) {
        if (!source_work(ctx,body->node)) return false;
        XrXirOp op=body->recipes[i].instruction.op;
        if (op!=XR_XIR_CONST_INT && op!=XR_XIR_COPY && op!=XR_XIR_RETURN) return true;
    }
    function->error_type=XR_XIR_NEVER;
    return true;
}
