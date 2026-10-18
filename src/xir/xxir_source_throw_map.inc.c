/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_throw_map.inc.c - Map sparse throw observations with region values
 */
static bool source_throw_map(SourceContext *ctx,const uint32_t *map,uint32_t value_count,uint32_t parameters) {
    SourceFunction *body=&ctx->bodies[ctx->function];
    if (!body->throw_count) return !body->throw_recipes ||
        source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source throw count is invalid");
    XrXirSourceDebugThrow *sites=source_alloc(ctx,body->throw_count,sizeof(*sites));if (!sites) return false;
    uint32_t count=0;
    for (const SourceThrowRecipe *recipe=body->throw_recipes;recipe;recipe=recipe->next) {
        if (!source_work_units(ctx,NULL,sizeof(*recipe))) return false;
        uint32_t instruction=0,operand=0;
        if (count==body->throw_count || recipe->instruction>=body->count ||
            (uint64_t)parameters+recipe->instruction>=value_count ||
            !source_region_id(map,value_count,parameters+recipe->instruction,&instruction) || instruction<parameters ||
            !source_region_id(map,value_count,recipe->operand,&operand))
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source throw has a foreign region identity");
        instruction-=parameters;uint32_t at=count;
        while (at && sites[at-1].instruction>instruction) {
            if (!source_work(ctx,NULL)) return false;
            sites[at]=sites[at-1];--at;
        }
        if (at && sites[at-1].instruction==instruction)
            return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source throw observation is duplicated");
        sites[at]=(XrXirSourceDebugThrow){instruction,operand};++count;
    }
    if (count!=body->throw_count) return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source throw count is invalid");
    body->debug_throws=sites;return true;
}
