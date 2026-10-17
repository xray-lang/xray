/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_edge_occupancy_cases.h - Dense complete construction keys
 */
#ifndef XIR_INVOCATION_EDGE_OCCUPANCY_CASES_H
#define XIR_INVOCATION_EDGE_OCCUPANCY_CASES_H
static uint64_t edge_occupancy_receiving_work;
static size_t edge_occupancy_observed;
static XrCompileResources *edge_occupancy_sampling;
static void edge_occupancy_observe(void *pointer) {
    (void)pointer;CHECK(edge_occupancy_sampling && !edge_occupancy_observed);
    ++edge_occupancy_observed;edge_occupancy_receiving_work=edge_occupancy_sampling->stats.work;
}
static EffectInvocationEdgeKey edge_occupancy_key(const EffectInvocationEdge *row) {
    return (EffectInvocationEdgeKey){row->caller,row->target,row->instruction,row->producer,(uint32_t)row->latent};
}

/* Complete independent keys are looked up at high occupancy and after real
 * receiving. Capacity-sized field changes preserve the original hash bucket
 * for the four integer fields, but can never be mistaken for the full key. */
static XrXirStatus edge_occupancy_verify(EffectInvocationWorkspace *owner,
    const EffectInvocationEdge *oracle) {
    CHECK(owner->final.edge_count==24 && !memcmp(owner->final.edges,oracle,24*sizeof(*oracle)));
    size_t attempts=rp_attempts;
    for (uint32_t e=0;e<24;++e) {
        EffectInvocationEdgeKey key=edge_occupancy_key(&oracle[e]);size_t at=SIZE_MAX;
        XrXirStatus status=effect_invocation_edge_slot(owner,owner->edge_index,owner->edge_index_capacity,&key,&at);
        if (status!=XR_XIR_OK) return status;
        CHECK(owner->edge_index[at]==e+1);
        for (unsigned field=0;field<5;++field) {
            EffectInvocationEdgeKey absent=key;uint32_t step=(uint32_t)owner->edge_index_capacity;
            if (!field) absent.caller+=step;else if (field==1) absent.target+=step;
            else if (field==2) absent.instruction+=step;else if (field==3) absent.producer+=step;
            else absent.latent^=1;
            size_t position=SIZE_MAX;
            status=effect_invocation_edge_slot(owner,owner->edge_index,owner->edge_index_capacity,&absent,&position);
            if (status!=XR_XIR_OK) return status;
            CHECK(!owner->edge_index[position]);
        }
        uint32_t saved=owner->edge_index[at];owner->edge_index[at]=owner->final.edge_count+1;
        size_t output=SIZE_MAX;
        status=effect_invocation_edge_slot(owner,owner->edge_index,owner->edge_index_capacity,&key,&output);
        owner->edge_index[at]=saved;
        if (status==XR_XIR_BUDGET) return status;
        CHECK(status==XR_XIR_BAD_STRUCTURE && output==SIZE_MAX);
    }
    CHECK(rp_attempts==attempts && !memcmp(owner->final.edges,oracle,24*sizeof(*oracle)));return XR_XIR_OK;
}

/* This component owns real old/new index allocations but borrows synthetic
 * complete edge input. It never publishes semantic evidence or a ROOT fact. */
static XrXirStatus edge_occupancy_run(const XrXirCompileContext *context,unsigned mode) {
    EffectInvocationEdge rows[24],oracle[24];
    for (uint32_t e=0;e<24;++e) {
        rows[e]=(EffectInvocationEdge){.caller=e%3,.target=(e+1)%3,.instruction=e,
            .producer=e%4?UINT32_MAX:(e/4)%2,.next=e?e-1:UINT32_MAX,
            .consumer_next=e+1<24?e+1:UINT32_MAX,.latent=!!(e&1)};oracle[e]=rows[e];
    }
    EffectInvocationWorkspace owner={.work=context,.final={.edges=rows,.edge_capacity=24}};
    XrXirStatus status=XR_XIR_OK;
    owner.edge_index=xir_compile_calloc(context,32,sizeof(*owner.edge_index),&status);
    if (!owner.edge_index) return status;
    owner.edge_index_capacity=32;
    for (uint32_t e=0;e<24 && status==XR_XIR_OK;++e) {
        EffectInvocationEdgeKey key=edge_occupancy_key(&oracle[e]);size_t at=SIZE_MAX;
        status=effect_invocation_edge_slot(&owner,owner.edge_index,32,&key,&at);
        if (status!=XR_XIR_OK) break;
        CHECK(!owner.edge_index[at]);
        if (!xir_compile_work(context,sizeof(uint32_t))) { status=XR_XIR_BUDGET;break; }
        owner.edge_index[at]=e+1;owner.final.edge_count=e+1;
        if (e==22) {
            size_t attempts=rp_attempts;uint32_t *old=owner.edge_index;
            status=effect_invocation_edge_index_reserve(&owner);
            if (status==XR_XIR_OK) CHECK(owner.edge_index==old && owner.edge_index_capacity==32 && rp_attempts==attempts);
        }
    }
    if (status==XR_XIR_OK) status=edge_occupancy_verify(&owner,oracle);
    if (status!=XR_XIR_OK) goto done;
    uint32_t before[32];memcpy(before,owner.edge_index,sizeof(before));uint32_t *old=owner.edge_index;
    RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(context).live_bytes;size_t attempts=rp_attempts;
    if (mode==1 || mode==3) {
        CHECK(!rp_allocation_observer);edge_occupancy_sampling=context->resources;edge_occupancy_observed=0;
        rp_allocation_observer=edge_occupancy_observe;
    }
    if (mode==2) { rp_fail_at=rp_attempts;rp_injected=false; }
    status=effect_invocation_edge_index_reserve(&owner);
    if (mode==1 || mode==3) { rp_allocation_observer=NULL;edge_occupancy_sampling=NULL; }
    if (mode==2) {
        rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==attempts+1 &&
            owner.edge_index==old && owner.edge_index_capacity==32 && !memcmp(old,before,sizeof(before)) &&
            !memcmp(rows,oracle,sizeof(rows)) && rp_stats(context).live_bytes==held_live);rp_balanced(held);
        status=effect_invocation_edge_index_reserve(&owner);
    }
    if (mode==3) {
        CHECK(status==XR_XIR_BUDGET && edge_occupancy_observed==1 && rp_attempts==attempts+1 &&
            owner.edge_index==old && owner.edge_index_capacity==32 && !memcmp(old,before,sizeof(before)) &&
            !memcmp(rows,oracle,sizeof(rows)) && rp_stats(context).live_bytes==held_live);rp_balanced(held);
        size_t retry=rp_attempts;CHECK(effect_invocation_edge_index_reserve(&owner)==XR_XIR_BUDGET && rp_attempts==retry);
    }
    if (status==XR_XIR_OK) {
        CHECK(owner.edge_index_capacity==64 && owner.edge_index!=old && owner.final.edge_count==24);
        if (mode==1) CHECK(edge_occupancy_observed==1);
        status=edge_occupancy_verify(&owner,oracle);
    }
done:
    xr_compile_resources_free(owner.edge_index);return status;
}

static void invocation_edge_occupancy_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;CHECK(edge_occupancy_run(&context,1)==XR_XIR_OK);
    XrCompileResourceStats measured=rp_stats(&context);uint64_t receiving=edge_occupancy_receiving_work;
    rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(edge_occupancy_run(&context,2)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();XrCompileResourceLimits limits=rp_caps();limits.work=receiving+256+64;
    context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
    CHECK(edge_occupancy_run(&context,3)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();limits=rp_caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(edge_occupancy_run(&context,0)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Edge occupancy: actual32 slots and24 complete five-field keys before64-slot receiving, all120 absent field probes including96 same-home integer collisions and24 malformed buckets per walk preserve output; original edge bytes and old index survive actual1 receiving OOM/work cuts and same finite-owner retries, six axes%llu/%llu/%llu exact/minusone; no numeric semantic admission PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_EDGE_OCCUPANCY_CASES_H
