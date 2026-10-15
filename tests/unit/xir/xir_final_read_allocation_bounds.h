/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_final_read_allocation_bounds.h - Real allocation-time Final extent change
 */
#ifndef XIR_FINAL_READ_ALLOCATION_BOUNDS_H
#define XIR_FINAL_READ_ALLOCATION_BOUNDS_H
static EffectInvocationFinal *final_read_extent_target;
static uint32_t final_read_extent_callbacks,final_read_extent_mode;
static void final_read_extent_grow(void *memory) {
    CHECK(memory && final_read_extent_target && !final_read_extent_callbacks);
    ++final_read_extent_callbacks;rp_allocation_observer=NULL;
    switch (final_read_extent_mode) {
        case 0: ++final_read_extent_target->count;
            if (final_read_extent_target->capacity<final_read_extent_target->count)
                final_read_extent_target->capacity=final_read_extent_target->count;
            break;
        case 1: ++final_read_extent_target->edge_count;
            if (final_read_extent_target->edge_capacity<final_read_extent_target->edge_count)
                final_read_extent_target->edge_capacity=final_read_extent_target->edge_count;
            break;
        case 2: ++final_read_extent_target->capacity;break;
        case 3: ++final_read_extent_target->edge_capacity;break;
        case 4: final_read_extent_target->nodes=NULL;break;
        case 5: final_read_extent_target->edges=NULL;break;
        case 6: final_read_extent_target->material=NULL;break;
        case 7: final_read_extent_target->physical=NULL;break;
        case 8: final_read_extent_target->roots=NULL;break;
        case 9: final_read_extent_target->root_spaces=NULL;break;
        default: CHECK(false);
    }
}
static void final_read_allocation_bounds(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate,XrXirEffects *published) {
    EffectInvocationFinal *final=certificate->final;
    EffectInvocationFinal before_final=*final;
    for (uint32_t mode=0;mode<10;++mode) {
        RootParameterMark physical=rp_mark();XrCompileResourceStats before=rp_stats(context);
        CHECK(!rp_allocation_observer);
        final_read_extent_target=final;final_read_extent_callbacks=0;final_read_extent_mode=mode;
        rp_allocation_observer=final_read_extent_grow;EffectInvocationRead read={0};
        XrXirStatus status=effect_invocation_read_open(context,published,certificate->declared,final,&read);
        rp_allocation_observer=NULL;final_read_extent_target=NULL;
        *final=before_final;
        fprintf(stderr,"actual Final read allocation geometry mode%u callbacks%u status%u\n",
            mode,final_read_extent_callbacks,status);
        CHECK(final_read_extent_callbacks==1 && status==XR_XIR_BAD_STRUCTURE && !read.consumers);
        rp_balanced(physical);
        XrCompileResourceStats paid=rp_stats(context);
        CHECK(paid.work>before.work && paid.allocated_bytes>before.allocated_bytes && paid.live_bytes==before.live_bytes);
        read=(EffectInvocationRead){0};
        CHECK(effect_invocation_read_open(context,published,certificate->declared,final,&read)==XR_XIR_OK && read.consumers);
        effect_invocation_read_close(&read);rp_balanced(physical);
        CHECK(rp_stats(context).work>paid.work && rp_stats(context).allocated_bytes>paid.allocated_bytes);
    }
}
#endif // XIR_FINAL_READ_ALLOCATION_BOUNDS_H
