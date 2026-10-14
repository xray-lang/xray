/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_edge_index.inc.c - Construction-local exact edge keys
 */
typedef struct EffectInvocationEdgeKey {
    uint32_t caller,target,instruction,producer,latent;
} EffectInvocationEdgeKey;

/* Hashes locate buckets only. Every occupied bucket compares the complete
 * current key, and numeric edge indices survive the original array growth. */
static XrXirStatus effect_invocation_edge_slot(EffectInvocationWorkspace *owner,
    const uint32_t *slots,size_t capacity,const EffectInvocationEdgeKey *key,size_t *output) {
    if (!owner || !owner->work || !slots || !capacity || (capacity&(capacity-1)) ||
        capacity>SIZE_MAX/sizeof(*slots) || !key || !output ||
        (owner->final.edge_count && !owner->final.edges)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,14+sizeof(size_t)+2)) return XR_XIR_BUDGET;
    size_t at=(key->caller*UINT32_C(0x9e3779b1)^key->target*UINT32_C(0x85ebca6b)^
        key->instruction*UINT32_C(0xc2b2ae35)^key->producer*UINT32_C(0x27d4eb2f)^
        key->latent*UINT32_C(0x165667b1))&(capacity-1);
    for (size_t probe=0;probe<capacity;++probe) {
        if (!xir_compile_work(owner->work,2)) return XR_XIR_BUDGET;
        uint32_t slot=slots[at];
        if (!slot) {
            if (!xir_compile_work(owner->work,sizeof(*output))) return XR_XIR_BUDGET;
            *output=at;return XR_XIR_OK;
        }
        if (slot-1>=owner->final.edge_count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationEdge *edge=&owner->final.edges[slot-1];
        if (!xir_compile_work(owner->work,5)) return XR_XIR_BUDGET;
        if (edge->caller==key->caller && edge->target==key->target &&
            edge->instruction==key->instruction && edge->producer==key->producer &&
            (uint32_t)edge->latent==key->latent) {
            if (!xir_compile_work(owner->work,sizeof(*output))) return XR_XIR_BUDGET;
            *output=at;return XR_XIR_OK;
        }
        if (!xir_compile_work(owner->work,2+sizeof(at))) return XR_XIR_BUDGET;
        at=(at+1)&(capacity-1);
    }
    return XR_XIR_BAD_STRUCTURE;
}

/* A replacement remains private until every old edge has been reinserted.
 * Failure leaves the old index and all graph chains owned and unchanged. */
static XrXirStatus effect_invocation_edge_index_reserve(EffectInvocationWorkspace *owner) {
    if (!!owner->edge_index!=!!owner->edge_index_capacity ||
        (owner->final.edge_count && !owner->edge_index)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,4)) return XR_XIR_BUDGET;
    if (owner->edge_index_capacity && owner->final.edge_count<owner->edge_index_capacity/2)
        return XR_XIR_OK;
    if (owner->edge_index_capacity>SIZE_MAX/2) return XR_XIR_BUDGET;
    if (!xir_compile_work(owner->work,sizeof(size_t)+2)) return XR_XIR_BUDGET;
    size_t capacity=owner->edge_index_capacity?owner->edge_index_capacity*2:16;
    if (capacity>SIZE_MAX/sizeof(uint32_t)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint32_t *slots=xir_compile_calloc(owner->work,capacity,sizeof(*slots),&status);
    if (!slots) return status;
    for (uint32_t e=0;e<owner->final.edge_count && status==XR_XIR_OK;++e) {
        const EffectInvocationEdge *edge=&owner->final.edges[e];
        if (!xir_compile_work(owner->work,sizeof(EffectInvocationEdgeKey)+sizeof(size_t))) {
            status=XR_XIR_BUDGET;break;
        }
        EffectInvocationEdgeKey key={edge->caller,edge->target,edge->instruction,
            edge->producer,(uint32_t)edge->latent};
        size_t at=0;status=effect_invocation_edge_slot(owner,slots,capacity,&key,&at);
        if (status!=XR_XIR_OK) break;
        if (slots[at]) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (!xir_compile_work(owner->work,sizeof(*slots))) { status=XR_XIR_BUDGET;break; }
        slots[at]=e+1;
    }
    if (status==XR_XIR_OK && !xir_compile_work(owner->work,
        sizeof(owner->edge_index)+sizeof(owner->edge_index_capacity))) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(slots);return status; }
    xr_compile_resources_free(owner->edge_index);
    owner->edge_index=slots;owner->edge_index_capacity=capacity;return XR_XIR_OK;
}
