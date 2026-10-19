/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_scratch.inc.c - Request-owned replay storage
 *
 * KEY CONCEPT:
 *   Storage reuse does not retain a proof. Simultaneously live replays have
 *   separate blocks, charged to the same compiler resource owner.
 */
static XrXirStatus effect_invocation_scratch_claim(const EffectInvocationRead *owner,
    size_t bytes,EffectInvocationScratch **output) {
    if (!owner || !xir_compile_context_valid(owner->work) ||
        owner->replay_scratch!=&owner->scratch_head || !output || *output)
        return XR_XIR_BAD_STRUCTURE;
    if (!bytes) return XR_XIR_OK;
    if (bytes>SIZE_MAX-sizeof(EffectInvocationScratch)) return XR_XIR_BUDGET;
    EffectInvocationScratch **slot=owner->replay_scratch;
    while (*slot) {
        if (!xir_compile_work(owner->work,3)) return XR_XIR_BUDGET;
        if ((*slot)->resources!=owner->work->resources) return XR_XIR_BAD_STRUCTURE;
        if (!(*slot)->claimed) break;
        slot=&(*slot)->next;
    }
    EffectInvocationScratch *memory=*slot;
    if (!memory || memory->capacity<bytes) {
        XrXirStatus status=XR_XIR_OK;
        EffectInvocationScratch *replacement=xir_compile_alloc(owner->work,sizeof(*memory)+bytes,&status);
        if (!replacement) return status;
        if (!xir_compile_work(owner->work,sizeof(*replacement))) {
            xr_compile_resources_free(replacement);return XR_XIR_BUDGET;
        }
        *replacement=(EffectInvocationScratch){.next=memory?memory->next:NULL,
            .resources=owner->work->resources,.capacity=bytes};
        *slot=replacement;xr_compile_resources_free(memory);memory=replacement;
    }
    if (!xir_compile_work(owner->work,1)) return XR_XIR_BUDGET;
    memory->claimed=true;*output=memory;return XR_XIR_OK;
}

static void effect_invocation_scratch_release(const EffectInvocationRead *owner,
    EffectInvocationScratch *memory) {
    if (!owner || owner->replay_scratch!=&owner->scratch_head || !memory) return;
    for (EffectInvocationScratch **slot=owner->replay_scratch;*slot;slot=&(*slot)->next) {
        if (*slot!=memory) continue;
        /* Large one-off bodies do not extend their peak into later replays. */
        if (memory->capacity>4096) {
            *slot=memory->next;xr_compile_resources_free(memory);
        } else memory->claimed=false;
        return;
    }
}

static void effect_invocation_scratch_free(const EffectInvocationRead *owner) {
    if (!owner || owner->replay_scratch!=&owner->scratch_head) return;
    while (*owner->replay_scratch) {
        EffectInvocationScratch *next=(*owner->replay_scratch)->next;
        xr_compile_resources_free(*owner->replay_scratch);*owner->replay_scratch=next;
    }
}
