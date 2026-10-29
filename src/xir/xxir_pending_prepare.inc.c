/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_pending_prepare.inc.c - Closed observations retain complete declarations
 *
 * KEY CONCEPT:
 *   Discovery owns mutable descriptor heads and preserves the genuine Source
 *   prefix. No incomplete function graph can become a Checked artifact.
 */
static XrXirStatus spec_pending_seed(SpecContext *c) {
    const XrXirTypes *source=c->source->types;c->types=*source;
    XrXirTypeNode *nodes=spec_alloc(c,source->count,sizeof(*nodes));
    c->closed.node_cache=spec_alloc(c,source->count,sizeof(*c->closed.node_cache));
    if (c->diagnostic.status!=XR_XIR_OK) return c->diagnostic.status;
    if (!((c)->diagnostic.status == XR_XIR_OK)) return c->diagnostic.status;
    if (source->count) memcpy(nodes,source->nodes,(size_t)source->count*sizeof(*nodes));
    c->types.nodes=nodes;c->node_capacity=source->count;
    for (uint32_t i=0;i<source->count;++i) {
        if (!((c)->diagnostic.status == XR_XIR_OK)) return c->diagnostic.status;
        if (!source->nodes[i].parameter_span) c->closed.node_cache[i]=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+i);
    }
    return XR_XIR_OK;
}
static XrXirStatus spec_pending_arguments(SpecContext *c) {
    const SpecPendingPoint *point=c->pending;
    uint32_t helper=point->site.helper;
    if (!point->count || !point->borrowed_arguments || helper>=c->source->function_count ||
        !c->source->generics || c->source->generics[helper].parameter_count!=point->count ||
        c->source->functions[helper].parameter_count) return XR_XIR_BAD_STRUCTURE;
    const XrXirTypes *types=c->types.count || c->types.nominals || c->types.interfaces?&c->types:NULL;
    XrXirModule actual={XR_XIR_CHECKED,c->functions,c->count,NULL,NULL,types,NULL,XR_XIR_PROGRAM,NULL};
    XrXirProofContext proof={&actual,{XR_XIR_CONTEXT_CLOSED,0,0}};
    for (uint32_t a=0;a<point->count;++a) {
        if (!((c)->diagnostic.status == XR_XIR_OK)) return c->diagnostic.status;
        XrXirType type=point->borrowed_arguments[a];
        XrXirStatus status=xr_xir_compile_type_expression_shape(&c->remaining,types,type,0);
        if (status!=XR_XIR_OK) return status;
        XrXirConstraintUse use={c->source,{XR_XIR_CONTEXT_FUNCTION,helper,0},a,point->borrowed_arguments,point->count};
        status=xr_xir_compile_constraints_prove(&c->remaining,&proof,&use);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus spec_pending_receive(SpecContext *c) {
    XrXirStatus status=spec_nominal_fields(c);
    const XrXirTypes *types=c->types.count || c->types.nominals || c->types.interfaces?&c->types:NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_types_structure_verify(&c->remaining,types);
    if (status==XR_XIR_OK) status=spec_pending_arguments(c);
    if (status==XR_XIR_OK) status=xr_xir_compile_types_clone(&c->remaining,types, NULL,&c->pending->types);
    if (status==XR_XIR_OK) c->pending->arguments=xir_compile_copy(&c->remaining,c->pending->borrowed_arguments,
        (size_t)c->pending->count*sizeof(*c->pending->arguments),&status);
    return status;
}
