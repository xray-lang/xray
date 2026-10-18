/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_node_index.inc.c - Owned numeric body buckets for exact keys
 */
/* Buckets locate possible equations only. Root identity, dimensions and every
 * physical input word remain the complete key; no effect result is indexed. */
static XrXirStatus effect_invocation_node_index_new(EffectInvocationOwner *owner) {
    if (!owner || !xir_compile_context_valid(owner->work) || owner->body_heads ||
        owner->body_head_count || owner->count || !effect_body_material_shape(owner->material) ||
        owner->body.domain.function_count!=effect_body_count(owner->material))
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=owner->body.domain.function_count;
    if (!count || (uint64_t)count>SIZE_MAX/sizeof(uint32_t)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint32_t *heads=xir_compile_alloc(owner->work,(size_t)count*sizeof(*heads),&status);
    if (!heads) return status;
    if (!xir_compile_work(owner->work,(uint64_t)count*sizeof(*heads)+
        sizeof(owner->body_heads)+sizeof(owner->body_head_count))) {
        xr_compile_resources_free(heads);return XR_XIR_BUDGET;
    }
    for (uint32_t f=0;f<count;++f) heads[f]=UINT32_MAX;
    owner->body_heads=heads;owner->body_head_count=count;return XR_XIR_OK;
}

/* Strictly descending numeric links survive node-array growth and make every
 * bucket finite. Exact coverage is independently checked before sealing and
 * during later closed-equation validation, without a cached permission bit. */
static XrXirStatus effect_invocation_node_index_verify(const XrXirCompileContext *work,
    const EffectInvocationOwner *owner) {
    if (!owner || !xir_compile_context_valid(work) || !owner->body_heads || !owner->nodes ||
        !effect_body_material_shape(owner->material) || owner->material->resources!=work->resources ||
        owner->body_head_count!=owner->body.domain.function_count ||
        owner->body_head_count!=effect_body_count(owner->material) ||
        owner->count>owner->capacity) return XR_XIR_BAD_STRUCTURE;
    uint64_t linked=0;
    for (uint32_t f=0;f<owner->body_head_count;++f) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        uint32_t previous=UINT32_MAX;
        for (uint32_t n=owner->body_heads[f];n!=UINT32_MAX;n=owner->nodes[n].key_next) {
            if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
            if (n>=owner->count || n>=previous || owner->nodes[n].body!=f ||
                ++linked>owner->count) return XR_XIR_BAD_STRUCTURE;
            previous=n;
        }
    }
    return linked==owner->count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
