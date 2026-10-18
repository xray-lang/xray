/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_hash_query.inc.c - Independent native hash display type storage
 */
/* Display schemas append to a separate query pool after executable admission.
 * The checked pool and every existing executable type identity stay unchanged. */
typedef struct SourceHashQueryTypes { XrXirTypes *types; uint32_t capacity; } SourceHashQueryTypes;
static bool source_hash_query_intern(SourceContext *ctx,SourceHashQueryTypes *query,
    XrXirTypeNode node,XrXirType *output) {
    uint32_t span=xr_xir_type_span(query->types,node.kind==XR_XIR_TYPE_CALLABLE?node.result:node.element);
    for (uint32_t p=0;p<node.parameter_count;++p) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t child=xr_xir_type_span(query->types,node.parameters[p].type);
        if (child>span) span=child;
    }
    node.parameter_span=span;
    for (uint32_t i=0;i<query->types->count;++i) {
        if (!source_work(ctx,NULL)) return false;
        const XrXirTypeNode *old=&query->types->nodes[i];
        if (old->kind!=node.kind || old->element!=node.element || old->result!=node.result ||
            old->flags!=node.flags || old->parameter_count!=node.parameter_count) continue;
        bool same=true;
        for (uint32_t p=0;p<node.parameter_count;++p) {
            if (!source_work(ctx,NULL)) return false;
            if (old->parameters[p].type!=node.parameters[p].type ||
                old->parameters[p].mode!=node.parameters[p].mode) same=false;
        }
        if (same) { *output=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);return true; }
    }
    uint32_t limit=XR_XIR_CONSTRUCTED_TYPE_LIMIT-XR_XIR_CONSTRUCTED_TYPE_BASE;
    if (query->types->count>=limit)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"native query type identities exhausted");
    if (query->types->count==query->capacity) {
        uint32_t capacity=query->capacity>limit/2?limit:query->capacity?query->capacity*2:8;
        XrXirTypeNode *nodes=source_alloc(ctx,capacity,sizeof(*nodes));
        if (!nodes || !source_copy_bytes(ctx,NULL,nodes,query->types->nodes,query->types->count*sizeof(*nodes))) return false;
        query->types->nodes=nodes;query->capacity=capacity;
    }
    ((XrXirTypeNode *)query->types->nodes)[query->types->count]=node;
    *output=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+query->types->count++);return true;
}
static bool source_hash_query_projection(SourceContext *ctx,SourceHashQueryTypes *query,
    XrXirType key,XrXirType element,bool entries,XrXirType *output) {
    if (entries) {
        XrXirCallableParameter *fields=source_alloc(ctx,2,sizeof(*fields));
        if (!fields) return false;
        fields[0]=(XrXirCallableParameter){key,0};fields[1]=(XrXirCallableParameter){element,0};
        if (!source_hash_query_intern(ctx,query,(XrXirTypeNode){.kind=XR_XIR_TYPE_TUPLE,
            .parameters=fields,.parameter_count=2},&element)) return false;
    }
    return source_hash_query_intern(ctx,query,(XrXirTypeNode){.kind=XR_XIR_TYPE_ARRAY,.element=element},output);
}
