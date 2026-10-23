/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_default_error_bound.inc.c - Finite error bounds for local defaults
 *
 * KEY CONCEPT:
 *   Refine only this producer's real default helpers before their heads freeze.
 *   Only known recipes and actual nonthrowing declarations prove an empty bound.
 */
static bool source_constant_default_error_bound(SourceContext *ctx) {
    uint32_t function=ctx->function;
    if (ctx->bodies[function].checked_library || ctx->bodies[function].region_sealed ||
        ctx->functions[function].error_type!=XR_XIR_ERROR) return true;
    const XrXirFunctionIdentity *identity=&ctx->identities[function];
    bool field=ctx->bodies[function].node && ctx->bodies[function].node->type==AST_FIELD_DECL &&
        identity->nominal_owner && identity->method_kind==XR_XIR_MEMBER_HELPER;
    /* Only parameter-default construction sets default_expression. An ordinary
     * declaration, module initializer or implicit constructor is not this role. */
    if ((!ctx->bodies[function].default_expression && !field) || !ctx->bodies[function].count) return true;
    for (uint32_t i=0;i<ctx->bodies[function].count;++i) {
        if (!source_work(ctx,ctx->bodies[function].node)) return false;
        XrXirInstruction op=ctx->bodies[function].recipes[i].instruction;
        bool empty=false;
        if (!source_recipe_never(ctx,&op,i,&empty)) return false;
        if (!empty) return true;
    }
    ctx->functions[function].error_type=XR_XIR_NEVER;
    return true;
}
