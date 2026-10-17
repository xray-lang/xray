/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_final_vectors.inc.c - Exact immutable vector extents
 *
 * KEY CONCEPT:
 *   Private copies retain every actual node and edge. Child owners transfer
 *   only at final publication. Direct copies preserve both inputs on failure;
 *   private drivers may consume complete prefixes with one valid cleanup owner.
 */
static void effect_invocation_final_vectors_release(const EffectInvocationFinal *discard,
    const EffectInvocationFinal *keep) {
    if (discard->nodes!=keep->nodes) xr_compile_resources_free(discard->nodes);
    if (discard->edges!=keep->edges) xr_compile_resources_free(discard->edges);
}

static XrXirStatus effect_invocation_final_vectors_binding(const XrXirCompileContext *work,
    const EffectInvocationFinal *source,EffectInvocationFinal *target) {
    if (!xir_compile_context_valid(work) || !source || !target || source==target ||
        !source->material || source->material->resources!=work->resources ||
        source->count>source->capacity || source->edge_count>source->edge_capacity ||
        !!source->nodes!=!!source->capacity || !!source->edges!=!!source->edge_capacity ||
        target->material!=source->material || target->nodes!=source->nodes || target->edges!=source->edges ||
        target->count!=source->count || target->capacity!=source->capacity ||
        target->edge_count!=source->edge_count || target->edge_capacity!=source->edge_capacity)
        return XR_XIR_BAD_STRUCTURE;
    if ((uint64_t)source->count>SIZE_MAX/sizeof(*source->nodes) ||
        (uint64_t)source->edge_count>SIZE_MAX/sizeof(*source->edges)) return XR_XIR_BUDGET;
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_final_vectors_copy(const XrXirCompileContext *work,
    const EffectInvocationFinal *source,EffectInvocationFinal *target) {
    XrXirStatus status=effect_invocation_final_vectors_binding(work,source,target);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationFinal copied=*source;
    if (!xir_compile_work(work,sizeof(copied))) return XR_XIR_BUDGET;
    if (source->count<source->capacity) {
        size_t bytes=(size_t)source->count*sizeof(*source->nodes);
        copied.nodes=bytes?xir_compile_alloc(work,bytes,&status):NULL;
        if (status==XR_XIR_OK && !xir_compile_work(work,bytes)) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK && bytes) memcpy(copied.nodes,source->nodes,bytes);
        copied.capacity=source->count;
    }
    if (status==XR_XIR_OK && source->edge_count<source->edge_capacity) {
        size_t bytes=(size_t)source->edge_count*sizeof(*source->edges);
        copied.edges=bytes?xir_compile_alloc(work,bytes,&status):NULL;
        if (status==XR_XIR_OK && !xir_compile_work(work,bytes)) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK && bytes) memcpy(copied.edges,source->edges,bytes);
        copied.edge_capacity=source->edge_count;
    }
    if (status==XR_XIR_OK && !xir_compile_work(work,2*sizeof(void *)+2*sizeof(uint32_t)))
        status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) {
        effect_invocation_final_vectors_release(&copied,source);return status;
    }
    target->nodes=copied.nodes;target->capacity=copied.capacity;
    target->edges=copied.edges;target->edge_capacity=copied.edge_capacity;
    return XR_XIR_OK;
}

/* The enclosing driver destroys this private owner on every failure. Each
 * complete vector moves to both owners before its superseded storage dies;
 * a later refusal leaves one valid cleanup owner for the received prefix. */
static XrXirStatus effect_invocation_final_vectors_consume(const XrXirCompileContext *work,
    EffectInvocationFinal *source,EffectInvocationFinal *target) {
    XrXirStatus status=effect_invocation_final_vectors_binding(work,source,target);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationFinal copied=*source;
    if (!xir_compile_work(work,sizeof(copied))) return XR_XIR_BUDGET;
    if (source->count<source->capacity) {
        size_t bytes=(size_t)source->count*sizeof(*source->nodes);
        copied.nodes=bytes?xir_compile_alloc(work,bytes,&status):NULL;
        if (status==XR_XIR_OK && !xir_compile_work(work,bytes)) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK && bytes) memcpy(copied.nodes,source->nodes,bytes);
        if (status==XR_XIR_OK && !xir_compile_work(work,2*(sizeof(void *)+sizeof(uint32_t))))
            status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) {
            EffectInvocationNode *old=source->nodes;
            source->nodes=target->nodes=copied.nodes;
            source->capacity=target->capacity=copied.capacity=source->count;
            xr_compile_resources_free(old);
        }
    }
    if (status==XR_XIR_OK && source->edge_count<source->edge_capacity) {
        size_t bytes=(size_t)source->edge_count*sizeof(*source->edges);
        copied.edges=bytes?xir_compile_alloc(work,bytes,&status):NULL;
        if (status==XR_XIR_OK && !xir_compile_work(work,bytes)) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK && bytes) memcpy(copied.edges,source->edges,bytes);
        if (status==XR_XIR_OK && !xir_compile_work(work,2*(sizeof(void *)+sizeof(uint32_t))))
            status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) {
            EffectInvocationEdge *old=source->edges;
            source->edges=target->edges=copied.edges;
            source->edge_capacity=target->edge_capacity=copied.edge_capacity=source->edge_count;
            xr_compile_resources_free(old);
        }
    }
    if (status!=XR_XIR_OK) effect_invocation_final_vectors_release(&copied,source);
    return status;
}
