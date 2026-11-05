/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_handler_map.inc.c - Seal explicit selector and route coordinates
 */
typedef struct SourceHandlerMap {
    const uint32_t *values;
    uint32_t count, parameters;
} SourceHandlerMap;
static bool source_handler_instruction(const SourceHandlerMap *map,uint32_t old,uint32_t *output) {
    uint32_t value=0;
    if ((uint64_t)map->parameters+old>=map->count ||
        !source_region_id(map->values,map->count,map->parameters+old,&value) || value<map->parameters) return false;
    *output=value-map->parameters;return true;
}
static bool source_handler_selector(SourceContext *ctx,const SourceHandlerMap *map,
    const XrXirSourceDebugSelector *input,XrXirSourceDebugSelector *output) {
    if (input->first>input->accepted ||
        !source_handler_instruction(map,input->first,&output->first) ||
        !source_handler_instruction(map,input->accepted,&output->accepted)) return false;
    /* An interval is published only if its complete instruction sequence
     * survives sealing unchanged. A future emitter must provide new facts. */
    for (uint32_t i=input->first;i<input->accepted;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        uint32_t mapped=0;
        if (!source_handler_instruction(map,i,&mapped) ||
            mapped!=(uint64_t)output->first+i-input->first) return false;
    }
    if ((uint64_t)output->first+input->accepted-input->first!=output->accepted) return false;
    output->rejected=UINT32_MAX;
    return input->rejected==UINT32_MAX || source_handler_instruction(map,input->rejected,&output->rejected);
}
static bool source_handler_map(SourceContext *ctx,const uint32_t *map,uint32_t count,uint32_t parameters) {
    SourceFunction *body=&ctx->bodies[ctx->function];SourceHandlerMap mapping={map,count,parameters};
    if (!!body->handler_count!=!!body->handler_recipes || !!body->route_count!=!!body->route_recipes)
        return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source handler counts are invalid");
    XrXirSourceDebugHandler *handlers=body->handler_count ? source_alloc(ctx,body->handler_count,sizeof(*handlers)) : NULL;
    XrXirSourceDebugRoute *routes=body->route_count ? source_alloc(ctx,body->route_count,sizeof(*routes)) : NULL;
    if ((body->handler_count && !handlers) || (body->route_count && !routes)) return false;
    uint32_t used=0;
    for (const SourceHandlerRecipe *recipe=body->handler_recipes;recipe;recipe=recipe->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (++used>body->handler_count || !recipe->identity || recipe->identity>body->handler_count ||
            handlers[recipe->identity-1].selector_count || !recipe->selector_count || !recipe->selectors ||
            (recipe->parent && (recipe->parent->identity<=recipe->identity || recipe->parent->identity>body->handler_count))) goto invalid;
        XrXirSourceDebugHandler *handler=&handlers[recipe->identity-1];
        handler->parent=recipe->parent ? recipe->parent->identity : 0;handler->propagation=UINT32_MAX;
        handler->propagation_input=recipe->propagation_edge ? recipe->propagation_edge->input : UINT32_MAX;
        if (recipe->parent && handler->propagation_input==UINT32_MAX) goto invalid;
        if (!source_region_id(map,count,recipe->error,&handler->error) ||
            (recipe->propagation!=UINT32_MAX && !source_handler_instruction(&mapping,recipe->propagation,&handler->propagation))) goto invalid;
        XrXirSourceDebugSelector *selectors=source_alloc(ctx,recipe->selector_count,sizeof(*selectors));if (!selectors) return false;
        for (uint32_t i=0;i<recipe->selector_count;++i)
            if (!source_handler_selector(ctx,&mapping,&recipe->selectors[i],&selectors[i])) goto invalid;
        handler->selectors=selectors;handler->selector_count=recipe->selector_count;
    }
    if (used!=body->handler_count) goto invalid;
    used=0;
    for (const SourceRouteRecipe *recipe=body->route_recipes;recipe;recipe=recipe->next) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        uint32_t instruction=0,edge=0,error=0;
        if (used==body->route_count || !recipe->handler || !recipe->handler->identity || !recipe->flow ||
            recipe->flow->input==UINT32_MAX || recipe->flow->jump!=recipe->edge || recipe->flow->value.id!=recipe->error ||
            recipe->handler->identity>body->handler_count || !source_handler_instruction(&mapping,recipe->instruction,&instruction) ||
            !source_handler_instruction(&mapping,recipe->edge,&edge) || !source_region_id(map,count,recipe->error,&error)) goto invalid;
        uint32_t at=used;
        while (at && routes[at-1].instruction>instruction) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            routes[at]=routes[at-1];--at;
        }
        if (at && routes[at-1].instruction==instruction) goto invalid;
        routes[at]=(XrXirSourceDebugRoute){instruction,recipe->handler->identity,edge,error,recipe->flow->input};++used;
    }
    if (used!=body->route_count) goto invalid;
    body->debug_handlers=handlers;body->debug_routes=routes;return true;
invalid:
    return source_fail(ctx,NULL,XR_XIR_BAD_STRUCTURE,"source handler has a foreign region identity");
}
