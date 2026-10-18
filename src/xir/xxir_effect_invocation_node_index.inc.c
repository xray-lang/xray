/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_node_index.inc.c - Construction-local exact equation keys
 */
typedef struct EffectInvocationNodeKey {
    uint32_t root,body,parameters,instructions;
    uint64_t words,hash;
    const uint64_t *input;
} EffectInvocationNodeKey;

static XrXirStatus effect_invocation_node_key_hash(EffectInvocationWorkspace *owner,
    EffectInvocationNodeKey *key) {
    if (!owner || !owner->read.work || !key || key->words>SIZE_MAX/sizeof(*key->input) ||
        (key->words && !key->input)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,20)) return XR_XIR_BUDGET;
    uint64_t hash=UINT64_C(14695981039346656037),prime=UINT64_C(1099511628211);
    hash=(hash^key->root)*prime;hash=(hash^key->body)*prime;
    hash=(hash^key->parameters)*prime;hash=(hash^key->words)*prime;
    for (size_t w=0;w<(size_t)key->words;++w) {
        if (!xir_compile_work(owner->read.work,3)) return XR_XIR_BUDGET;
        hash=(hash^key->input[w])*prime;
    }
    hash^=hash>>33;hash*=UINT64_C(0xff51afd7ed558ccd);
    hash^=hash>>33;hash*=UINT64_C(0xc4ceb9fe1a85ec53);hash^=hash>>33;
    key->hash=hash;return XR_XIR_OK;
}

/* Hashes only locate buckets. Full origin rows decide equality; an altered
 * instruction span remains an inconsistency rather than a new equation. */
static XrXirStatus effect_invocation_node_slot(EffectInvocationWorkspace *owner,
    const uint32_t *slots,size_t capacity,const EffectInvocationNodeKey *key,size_t *output) {
    if (!owner || !owner->read.work || !slots || !capacity || (capacity&(capacity-1)) ||
        capacity>SIZE_MAX/sizeof(*slots) || !key || !output ||
        (key->words && !key->input) || key->words>SIZE_MAX/sizeof(*key->input) ||
        (owner->final.count && !owner->final.nodes)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,sizeof(size_t)+2)) return XR_XIR_BUDGET;
    size_t at=(size_t)key->hash&(capacity-1);
    for (size_t probe=0;probe<capacity;++probe) {
        if (!xir_compile_work(owner->read.work,2)) return XR_XIR_BUDGET;
        uint32_t slot=slots[at];
        if (!slot) { *output=at;return XR_XIR_OK; }
        if (slot-1>=owner->final.count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *node=&owner->final.nodes[slot-1];
        if (!xir_compile_work(owner->read.work,3)) return XR_XIR_BUDGET;
        if (node->root==key->root && node->body==key->body && node->parameter_count==key->parameters) {
            if (key->words && !node->input) return XR_XIR_BAD_STRUCTURE;
            bool same=true;
            for (size_t w=0;w<(size_t)key->words;++w) {
                if (!xir_compile_work(owner->read.work,1)) return XR_XIR_BUDGET;
                if (node->input[w]!=key->input[w]) { same=false;break; }
            }
            if (same) {
                if (!xir_compile_work(owner->read.work,1)) return XR_XIR_BUDGET;
                if (node->instruction_count!=key->instructions) return XR_XIR_BAD_STRUCTURE;
                *output=at;return XR_XIR_OK;
            }
        }
        if (!xir_compile_work(owner->read.work,2+sizeof(at))) return XR_XIR_BUDGET;
        at=(at+1)&(capacity-1);
    }
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_invocation_node_existing_key(EffectInvocationWorkspace *owner,
    uint32_t n,EffectInvocationNodeKey *key) {
    if (!owner || !key || n>=owner->final.count) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&owner->final.nodes[n];
    EffectInvocationBasis basis={0};XrXirStatus status=effect_invocation_basis(&owner->read,node->root,&basis);
    if (status!=XR_XIR_OK) return status;
    EffectBodyFunctionDimensions head={0};status=effect_invocation_dimensions(&owner->read,node->body,&head);
    if (status!=XR_XIR_OK) return status;
    if (node->parameter_count!=head.parameter_count || node->instruction_count!=head.instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    *key=(EffectInvocationNodeKey){node->root,node->body,node->parameter_count,node->instruction_count,
        (uint64_t)head.parameter_count*basis.row_words,0,node->input};
    return effect_invocation_node_key_hash(owner,key);
}

/* Only numeric node IDs survive growth. A failed replacement leaves the old
 * index and all owned equations intact; Final never receives this index. */
static XrXirStatus effect_invocation_node_index_reserve(EffectInvocationWorkspace *owner) {
    if (!owner || !owner->read.work || !!owner->node_index!=!!owner->node_index_capacity ||
        (owner->node_index_capacity&(owner->node_index_capacity-1)) ||
        (owner->final.count && !owner->final.nodes))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,4)) return XR_XIR_BUDGET;
    if (owner->node_index_capacity && owner->final.count<owner->node_index_capacity/2)
        return XR_XIR_OK;
    size_t capacity=owner->node_index_capacity?owner->node_index_capacity:8;
    do {
        if (capacity>SIZE_MAX/2) return XR_XIR_BUDGET;
        capacity*=2;
    } while (owner->final.count>=capacity/2);
    if (capacity>SIZE_MAX/sizeof(uint32_t)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint32_t *slots=xir_compile_calloc(owner->read.work,capacity,sizeof(*slots),&status);
    if (!slots) return status;
    for (uint32_t n=0;n<owner->final.count && status==XR_XIR_OK;++n) {
        EffectInvocationNodeKey key={0};status=effect_invocation_node_existing_key(owner,n,&key);
        size_t at=0;
        if (status==XR_XIR_OK) status=effect_invocation_node_slot(owner,slots,capacity,&key,&at);
        if (status!=XR_XIR_OK) break;
        if (slots[at]) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (!xir_compile_work(owner->read.work,sizeof(*slots))) { status=XR_XIR_BUDGET;break; }
        slots[at]=n+1;
    }
    if (status==XR_XIR_OK && !xir_compile_work(owner->read.work,
        sizeof(owner->node_index)+sizeof(owner->node_index_capacity))) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(slots);return status; }
    xr_compile_resources_free(owner->node_index);
    owner->node_index=slots;owner->node_index_capacity=capacity;return XR_XIR_OK;
}
