/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_region_seal.inc.c - Verify region expressions and publish owned observations
 */
static bool source_region_expressions(SourceContext *ctx,bool seal) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t expressions=0;
    for (SourceExpressionStorage *storage=body->expressions;storage;storage=storage->next) {
        if (!source_work(ctx,NULL)) return false;
        if (!storage->count || storage->count>32 || storage->count>body->expression_count-expressions)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression region storage is invalid");
        for (uint32_t e=0;e<storage->count;++e) {
            if (!source_work(ctx,NULL)) return false;
            SourceExpressionPlan *plan=&storage->expressions[e];
            if (plan->owner!=ctx->function || (seal && plan->state!=SOURCE_TERM_GROUND && plan->state!=SOURCE_TERM_DISCARDED) ||
                plan->identity>=body->expression_count || plan->entry.owner!=ctx->function ||
                (plan->state==SOURCE_TERM_GROUND && (plan->exit.owner!=ctx->function ||
                 plan->entry.identity>=body->block_count || plan->exit.identity>=body->block_count)) ||
                (plan->parent && plan->parent->owner!=ctx->function))
                return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression region is unresolved or has a foreign owner");
        }
        expressions+=storage->count;
    }
    if (expressions!=body->expression_count)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"expression region count is invalid");
    return true;
}
static bool source_region_seal(SourceContext *ctx,const uint32_t *map,uint32_t value_count,
    SourceRootValue *root_values,const XrXirSourceDebugPoint *points) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    uint32_t parameters=ctx->functions[ctx->function].parameter_count;
    if (!source_throw_map(ctx,map,value_count,parameters) || !source_handler_map(ctx,map,value_count,parameters)) return false;
    for (uint32_t i=0;i<body->debug_binding_count;++i) {
        if (!source_work_units(ctx,NULL,sizeof(*body->debug_bindings))) return false;
        XrXirSourceDebugBinding *binding=&body->debug_bindings[i];
        if (!(binding->flags & (XR_XIR_SOURCE_DEBUG_UNIT|XR_XIR_SOURCE_DEBUG_UNAVAILABLE)) &&
            !source_region_id(map,value_count,binding->value,&binding->value)) {
            char message[128];
            source_format(ctx,message,sizeof(message),"source debug function %u binding %u has invalid value %u",ctx->function,i,binding->value);
            source_fail(ctx,body->node,XR_XIR_BAD_STRUCTURE,message);return false;
        }
    }
    for (SourceRootQuery *query = ctx->root_queries; query; query = query->next) {
        if (!source_work(ctx,NULL)) return false;
        if (query->function == ctx->function && !source_region_id(map,value_count,query->value,&query->value)) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source region identity is invalid");
    }
    source_retire_array(ctx,body->operands);
    body->operands=NULL;body->operand_count=body->operand_capacity=0;
    body->region_sealed=true; body->root_values=root_values;body->debug_points=points;
    while (body->recipe_storage) {
        SourceRecipeStorage *storage=body->recipe_storage;body->recipe_storage=storage->next;
        source_release_private(storage);
    }
    body->expressions=NULL;body->expression_count=0;
    body->recipes=NULL;body->blocks=NULL;body->capacity=0;body->block_capacity=0;
    return true;
}
