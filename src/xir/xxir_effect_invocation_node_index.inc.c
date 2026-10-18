/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_node_index.inc.c - Temporary exact equation lookup by body
 *
 * KEY CONCEPT:
 *   Immutable body numbers select buckets in the sole node inventory. Every
 *   root and complete input still compares exactly; no lookup enters Final.
 */
typedef struct EffectInvocationNodeLookup {
    uint32_t root,body,parameters;
    const uint64_t *input;
    size_t words;
} EffectInvocationNodeLookup;

static XrXirStatus effect_invocation_node_find(EffectInvocationWorkspace *owner,
    const EffectInvocationNodeLookup *key,uint32_t *output) {
    if (!owner || !key || !output || (key->words && !key->input) ||
        !owner->node_index || owner->node_index_capacity!=owner->read.body.domain.function_count ||
        key->body>=owner->node_index_capacity || owner->final.count>owner->final.capacity ||
        (owner->final.count && (!owner->final.nodes || !owner->states))) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,4+sizeof(uint32_t))) return XR_XIR_BUDGET;
    for (uint32_t entry=owner->node_index[key->body];entry;) {
        if (!xir_compile_work(owner->read.work,5+sizeof(uint32_t))) return XR_XIR_BUDGET;
        uint32_t n=entry-1;
        if (n>=owner->final.count) return XR_XIR_BAD_STRUCTURE;
        const EffectInvocationNode *node=&owner->final.nodes[n];
        uint32_t next=owner->states[n].lookup_next;
        if (next>=entry || node->body!=key->body || node->parameter_count!=key->parameters)
            return XR_XIR_BAD_STRUCTURE;
        if (node->root==key->root) {
            if (key->words && !node->input) return XR_XIR_BAD_STRUCTURE;
            bool same=true;
            for (size_t w=0;w<key->words;++w) {
                if (!xir_compile_work(owner->read.work,1)) return XR_XIR_BUDGET;
                if (node->input[w]!=key->input[w]) same=false;
            }
            if (same) {
                if (!xir_compile_work(owner->read.work,sizeof(*output))) return XR_XIR_BUDGET;
                *output=n;return XR_XIR_OK;
            }
        }
        entry=next;
    }
    if (!xir_compile_work(owner->read.work,sizeof(*output))) return XR_XIR_BUDGET;
    *output=UINT32_MAX;return XR_XIR_OK;
}

/* Links descend through original node numbers. Array growth copies numeric
 * links only; no pointer to a movable node or stored permission is retained. */
static XrXirStatus effect_invocation_node_index_put(EffectInvocationWorkspace *owner,
    uint32_t *slots,size_t capacity,uint32_t n) {
    if (!owner || !slots || capacity!=owner->read.body.domain.function_count ||
        n>=owner->final.count || owner->final.count>owner->final.capacity ||
        !owner->final.nodes || !owner->states) return XR_XIR_BAD_STRUCTURE;
    uint32_t body=owner->final.nodes[n].body;
    if (body>=capacity || slots[body]>n) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->read.work,3+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
    owner->states[n].lookup_next=slots[body];slots[body]=n+1;return XR_XIR_OK;
}

static XrXirStatus effect_invocation_node_index_reserve(EffectInvocationWorkspace *owner) {
    if (!owner || !!owner->node_index!=!!owner->node_index_capacity ||
        owner->final.count>owner->final.capacity ||
        (owner->final.count && (!owner->final.nodes || !owner->states))) return XR_XIR_BAD_STRUCTURE;
    size_t capacity=owner->read.body.domain.function_count;
    if (!capacity || capacity>SIZE_MAX/sizeof(uint32_t)) return XR_XIR_BUDGET;
    if (!xir_compile_work(owner->read.work,4)) return XR_XIR_BUDGET;
    if (owner->node_index) return owner->node_index_capacity==capacity?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
    uint32_t count=owner->final.count;const EffectInvocationNode *nodes=owner->final.nodes;
    EffectInvocationState *states=owner->states;const XrXirCompileContext *work=owner->read.work;
    XrXirStatus status=XR_XIR_OK;
    uint32_t *slots=xir_compile_calloc(work,capacity,sizeof(*slots),&status);
    if (!slots) return status;
    if (owner->read.work!=work || owner->read.body.domain.function_count!=capacity ||
        owner->final.nodes!=nodes || owner->states!=states || owner->final.count!=count ||
        owner->node_index || owner->node_index_capacity) status=XR_XIR_BAD_STRUCTURE;
    for (uint32_t n=0;n<count && status==XR_XIR_OK;++n)
        status=effect_invocation_node_index_put(owner,slots,capacity,n);
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(owner->node_index)+sizeof(owner->node_index_capacity)))
        status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) {xr_compile_resources_free(slots);return status;}
    owner->node_index=slots;owner->node_index_capacity=capacity;return XR_XIR_OK;
}
