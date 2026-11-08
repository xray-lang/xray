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


static const XrXirTypes effect_empty_types={0};

/* The structural owner retains reusable stack capacity, never a type proof.
 * Its original ledger admits every fresh comparison and owns all blocks. */

/* Declaration metadata keeps its original source IDs during resolution. Each
 * descriptor and declaration child is copied; executable layout is excluded. */
static XrXirStatus effect_terms_seed(EffectTerms *pool, const XrXirTypes *source) {
    if (!source || !!source->nodes != !!source->count || pool->types.count || pool->owned_nominals || pool->owned_interfaces)
        return XR_XIR_BAD_STRUCTURE;
    XrXirTypeNode *nodes = source->count ? effect_terms_alloc(pool,source->count,sizeof(*nodes)) : NULL;
    if (source->count && !nodes) return pool->status;
    for (uint32_t i=0;i<source->count;++i) {

        XrXirTypeNode node=source->nodes[i];
        uint32_t slots=xr_xir_type_parameter_slots(&node);
        if (slots) {
            XrXirCallableParameter *parameters=effect_terms_alloc(pool,slots,sizeof(*parameters));
            if (!parameters) return pool->status;

            memcpy(parameters,node.parameters,(size_t)slots*sizeof(*parameters));
            node.parameters=parameters;
        }
        if (node.nominal.argument_count) {
            XrXirType *arguments=effect_terms_alloc(pool,node.nominal.argument_count,sizeof(*arguments));
            if (!arguments) return pool->status;

            memcpy(arguments,node.nominal.arguments,(size_t)node.nominal.argument_count*sizeof(*arguments));
            node.nominal.arguments=arguments;
        }
        /* Original projections are already checked in this coordinate space.
         * Marker proofs need their complete fields after Source retirement. */
        if(node.nominal.field_count) {
            XrXirType *fields=effect_terms_alloc(pool,node.nominal.field_count,sizeof(*fields));
            if(!fields)return pool->status;
            memcpy(fields,node.nominal.fields,(size_t)node.nominal.field_count*sizeof(*fields));
            node.nominal.fields=fields;
        }
        nodes[i]=node;
    }
    pool->types=(XrXirTypes){nodes,source->count,NULL,NULL};
    pool->capacity=source->count;
    XrXirStatus status=source->nominals ? xr_xir_compile_nominal_clone(pool->remaining,
        source->nominals,source,&pool->owned_nominals) : XR_XIR_OK;
    if (status==XR_XIR_OK && source->interfaces)
        status=xr_xir_compile_interfaces_copy_verified(pool->remaining,source->interfaces, NULL,&pool->owned_interfaces);
    if (status!=XR_XIR_OK) return status;
    pool->types.nominals=pool->owned_nominals;pool->types.interfaces=pool->owned_interfaces;
    return XR_XIR_OK;
}

/* Import in verified descriptor order without host recursion. The resulting
 * IDs belong only to this owner, even when the source pool has another order. */
