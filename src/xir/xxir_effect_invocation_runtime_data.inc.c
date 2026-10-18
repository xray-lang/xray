/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_runtime_data.inc.c - Current semantic coordinates in one sealed owner
 */
typedef struct EffectRuntimeKeyRead {
    const EffectInvocationCertificate *certificate;
    const XirEffectInvocationRequest *request;
    const EffectInvocationNode *from;
    EffectInvocationBasis basis;
    uint32_t node;
} EffectRuntimeKeyRead;

static XrXirStatus effect_runtime_key_layout(EffectRuntimeKeyRead *read,uint32_t parent) {
    const EffectInvocationFinal *owner=read->certificate->final;
    if (owner->material!=read->certificate->body || owner->count>owner->capacity ||
        !owner->nodes || parent>=owner->count || owner->capture.count>owner->capture.capacity ||
        !!owner->capture.facts!=!!owner->capture.capacity) return XR_XIR_BAD_STRUCTURE;
    read->from=&owner->nodes[parent];
    XrXirStatus status=effect_runtime_basis(read->certificate,read->from->root,&read->basis,&read->request->read);
    if (status!=XR_XIR_OK) return status;
    status=effect_runtime_work(&read->request->read,sizeof(read->node));
    if (status==XR_XIR_OK) read->node=parent;
    return status;
}

/* Data atoms have no causes. Identity and capture atoms were independently
 * proved before this immutable Final entered the Program. Their numeric
 * coordinates project the same complete row, with no reconstructed graph. */


static XrXirStatus effect_runtime_key_has(const EffectRuntimeKeyRead *read,uint32_t value,
    uint32_t word,uint32_t bit,bool *output) {
    if (word>=read->basis.row_words || bit>=64) return XR_XIR_BAD_STRUCTURE;
    return effect_runtime_capture_has(read->certificate,&read->request->read,
        (EffectCaptureFact){read->node,value,word,bit,0},output);
}
static XrXirStatus effect_runtime_key_fixed(const EffectRuntimeKeyRead *read,uint32_t value,bool *output) {
    return effect_runtime_capture_has(read->certificate,&read->request->read,
        (EffectCaptureFact){read->node,value,EFFECT_CAPTURE_FLAGS,EFFECT_CAPTURE_FIXED,0},output);
}

static XrXirStatus effect_runtime_key_span(const EffectRuntimeKeyRead *read,uint32_t value,
    uint32_t *first,uint32_t *last) {
    if (!first || !last) return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationFinal *final=read->certificate->final;
    uint32_t count=final->capture.count,levels=0;
    for (uint32_t remaining=count;remaining;remaining>>=1) ++levels;
    XrXirStatus status=effect_runtime_work(&read->request->read,
        12+(uint64_t)levels*6+2*sizeof(uint32_t));
    if (status!=XR_XIR_OK) return status;
    if (final->capture.count!=count || count>final->capture.capacity ||
        !!final->capture.facts!=!!final->capture.capacity || final->count>final->capacity ||
        !final->nodes || read->node>=final->count ||
        read->from!=&final->nodes[read->node]) return XR_XIR_BAD_STRUCTURE;
    uint32_t bounds[2]={0,0};
    for (uint32_t end=0;end<2;++end) {
        uint32_t lo=0,hi=count;
        while (lo<hi) {
            uint32_t mid=lo+(hi-lo)/2;const EffectCaptureFact *fact=&final->capture.facts[mid];
            bool before=fact->node<read->node || (fact->node==read->node &&
                (fact->value<value || (end && fact->value==value)));
            if (before) lo=mid+1;else hi=mid;
        }
        bounds[end]=lo;
    }
    *first=bounds[0];*last=bounds[1];return XR_XIR_OK;
}
