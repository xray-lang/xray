/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_materialization.inc.c - Owned requests for private literal data
 *
 * KEY CONCEPT:
 *   A request retains one already received constant, not its producer call.
 *   Its declaration is resolved through the current real Source inventory.
 */
struct SourceMaterialization {
    SourceMaterialization *next;
    SourceName *owner;
    AstNode *site;
    XrXirValue value;
    XrXirType type;
    uint32_t function,caller;
};
static bool source_materialization_receive(SourceContext *ctx,AstNode *node,
    const XrXirValue *value,XrXirType type,SourceValue *output,bool *handled) {
    *handled=false;
    const XrXirTypeNode *target=xr_xir_type_node(&ctx->types,type);
    if (!ctx->comptime_input || !target || target->kind!=XR_XIR_TYPE_NOMINAL)
        return true;
    uint32_t nominal=target->nominal.declaration;
    if (nominal>=ctx->nominals.count) return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"constant declaration is absent");
    if (ctx->identities[ctx->function].nominal_owner==nominal+1) return true;
    const XrXirNominalDeclaration *record=&ctx->nominals.declarations[nominal];
    bool restricted=false;
    for (uint32_t f=0;f<record->field_count;++f) {
        if (!source_work_units(ctx,node,sizeof(XrXirNominalField))) return false;
        restricted|=(record->fields[f].flags&(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED))!=0;
    }
    if (!restricted) return true;
    SourceName *owner=ctx->nominal_sources[nominal];
    if (!owner || owner->kind!=SOURCE_NOMINAL || owner->index!=nominal ||
        !owner->declaration || owner->declaration>ctx->query.declaration_count)
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"constant has no current declaration owner");
    const XrXirSourceDeclaration *declaration=&ctx->query.declarations[owner->declaration-1];
    if (declaration->kind!=XR_XIR_SOURCE_TYPE || declaration->range.module>=ctx->module_count ||
        (!owner->checked_library && !owner->node))
        return source_fail(ctx,node,XR_XIR_BAD_STRUCTURE,"constant declaration birth is incomplete");
    SourceComptimeNominalShape shape={0};
    if (!source_comptime_nominal_shape(ctx,node,value,type,&shape)) return false;
    if (ctx->materialization_begin>ctx->compile.limits.functions ||
        ctx->materialization_count>=ctx->compile.limits.functions-ctx->materialization_begin)
        return source_fail(ctx,node,XR_XIR_BUDGET,"constant materializer inventory exhausted");
    SourceMaterialization *request=source_alloc(ctx,1,sizeof(*request));
    if (!request || !source_work_units(ctx,node,sizeof(*request)+sizeof(XrXirValue))) return false;
    XrXirValueStatus copied=xr_xir_value_copy(value,&request->value);
    if (copied!=XR_XIR_VALUE_OK) return source_fail(ctx,node,
        copied==XR_XIR_VALUE_LIMIT || copied==XR_XIR_VALUE_REFCOUNT_LIMIT?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE,
        "constant materializer receiving failed");
    request->owner=owner;request->site=node;request->type=type;request->caller=ctx->function;
    request->function=ctx->materialization_begin+ctx->materialization_count++;
    if (ctx->materialization_tail) ctx->materialization_tail->next=request;else ctx->materializations=request;
    ctx->materialization_tail=request;
    *handled=true;
    /* Literal-only callees cannot throw. They remain direct edges even inside
     * a caller's catch region; ordinary effect checking revalidates the body. */
    return source_recipe_append(ctx,(XrXirInstruction){XR_XIR_CALL,type,{0},{0},request->function,{0}},output);
}
