/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_context_types.inc.c - Owned contextual type environments
 *
 * KEY CONCEPT:
 *   Original declaration IDs survive while every structural child belongs to
 *   this context owner. Cross-pool import uses verified descriptor order.
 */
#include "xxir_effect_context_domain.inc.c"

static const XrXirTypes effect_empty_types={0};

/* Declaration metadata keeps its original source IDs during resolution. Each
 * descriptor and declaration child is copied; executable layout is excluded. */
static XrXirStatus effect_terms_seed(EffectTerms *pool, const XrXirTypes *source) {
    if (!source || !!source->nodes != !!source->count || pool->types.count || pool->owned_nominals || pool->owned_interfaces)
        return XR_XIR_BAD_STRUCTURE;
    XrXirTypeNode *nodes = source->count ? effect_terms_alloc(pool,source->count,sizeof(*nodes)) : NULL;
    if (source->count && !nodes) return pool->status;
    for (uint32_t i=0;i<source->count;++i) {
        if (!xir_compile_work(pool->remaining,1+2*sizeof(XrXirTypeNode))) return XR_XIR_BUDGET;
        XrXirTypeNode node=source->nodes[i];
        if (node.parameter_count) {
            XrXirCallableParameter *parameters=effect_terms_alloc(pool,node.parameter_count,sizeof(*parameters));
            if (!parameters) return pool->status;
            if (!xir_compile_work(pool->remaining,(uint64_t)node.parameter_count*sizeof(*parameters)))
                return XR_XIR_BUDGET;
            memcpy(parameters,node.parameters,(size_t)node.parameter_count*sizeof(*parameters));
            node.parameters=parameters;
        }
        if (node.nominal.argument_count) {
            XrXirType *arguments=effect_terms_alloc(pool,node.nominal.argument_count,sizeof(*arguments));
            if (!arguments) return pool->status;
            if (!xir_compile_work(pool->remaining,(uint64_t)node.nominal.argument_count*sizeof(*arguments)))
                return XR_XIR_BUDGET;
            memcpy(arguments,node.nominal.arguments,(size_t)node.nominal.argument_count*sizeof(*arguments));
            node.nominal.arguments=arguments;
        }
        node.nominal.fields=NULL;node.nominal.field_count=0;
        nodes[i]=node;
    }
    pool->types=(XrXirTypes){nodes,source->count,NULL,NULL};
    pool->capacity=source->count;
    XrXirStatus status=source->nominals ? xr_xir_compile_nominal_clone(pool->remaining,
        source->nominals,source,&pool->owned_nominals) : XR_XIR_OK;
    if (status==XR_XIR_OK && source->interfaces)
        status=xr_xir_compile_interfaces_copy_verified(pool->remaining,source->interfaces,&pool->owned_interfaces);
    if (status!=XR_XIR_OK) return status;
    pool->types.nominals=pool->owned_nominals;pool->types.interfaces=pool->owned_interfaces;
    return XR_XIR_OK;
}

static XrXirStatus effect_terms_import_child(EffectTerms *pool,
    const XrXirType *cache, uint32_t before, XrXirType input, XrXirType *output) {
    if (!xir_compile_work(pool->remaining,1)) return XR_XIR_BUDGET;
    uint32_t id=(uint32_t)input;
    if (id>=XR_XIR_CONSTRUCTED_TYPE_BASE && id<XR_XIR_CONSTRUCTED_TYPE_LIMIT) {
        uint32_t index=id-XR_XIR_CONSTRUCTED_TYPE_BASE;
        if (index>=before) return XR_XIR_BAD_STRUCTURE;
        *output=cache[index];
    } else *output=input;
    return XR_XIR_OK;
}

/* Import in verified descriptor order without host recursion. The resulting
 * IDs belong only to this owner, even when the source pool has another order. */
static XrXirStatus effect_terms_import(EffectTerms *pool,
    const XrXirTypes *source, const XrXirType **output) {
    if (!source || !!source->nodes != !!source->count || !output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus domain=effect_terms_domain_match(pool,source);
    if (domain!=XR_XIR_OK) return domain;
    XrXirType *cache=source->count ? effect_terms_alloc(pool,source->count,sizeof(*cache)) : NULL;
    if (source->count && !cache) return pool->status;
    for (uint32_t i=0;i<source->count;++i) {
        if (!xir_compile_work(pool->remaining,1+sizeof(XrXirTypeNode))) return XR_XIR_BUDGET;
        XrXirTypeNode node=source->nodes[i];
        XrXirStatus status=XR_XIR_OK;
        if (node.kind==XR_XIR_TYPE_CALLABLE || node.kind==XR_XIR_TYPE_TUPLE) {
            XrXirCallableParameter *parameters=node.parameter_count ?
                effect_terms_alloc(pool,node.parameter_count,sizeof(*parameters)) : NULL;
            if (node.parameter_count && !parameters) return pool->status;
            for (uint32_t p=0;p<node.parameter_count && status==XR_XIR_OK;++p) {
                parameters[p].mode=node.parameters[p].mode;
                status=effect_terms_import_child(pool,cache,i,node.parameters[p].type,&parameters[p].type);
            }
            node.parameters=parameters;
            if (status==XR_XIR_OK && node.kind==XR_XIR_TYPE_CALLABLE)
                status=effect_terms_import_child(pool,cache,i,node.result,&node.result);
        } else if (node.kind==XR_XIR_TYPE_NOMINAL) {
            XrXirType *arguments=node.nominal.argument_count ?
                effect_terms_alloc(pool,node.nominal.argument_count,sizeof(*arguments)) : NULL;
            if (node.nominal.argument_count && !arguments) return pool->status;
            for (uint32_t a=0;a<node.nominal.argument_count && status==XR_XIR_OK;++a)
                status=effect_terms_import_child(pool,cache,i,node.nominal.arguments[a],&arguments[a]);
            node.nominal.arguments=arguments;
        } else status=effect_terms_import_child(pool,cache,i,node.element,&node.element);
        if (status!=XR_XIR_OK) return status;
        cache[i]=effect_term_intern(pool,node);
        if (pool->status!=XR_XIR_OK) return pool->status;
    }
    *output=cache;return XR_XIR_OK;
}
