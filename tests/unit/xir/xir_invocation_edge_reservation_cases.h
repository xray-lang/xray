/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_edge_reservation_cases.h - One actual bounded edge receiving
 */
#ifndef XIR_INVOCATION_EDGE_RESERVATION_CASES_H
#define XIR_INVOCATION_EDGE_RESERVATION_CASES_H
static uint64_t edge_reservation_prefix_live,edge_reservation_allocated_work;
static size_t edge_reservation_observed;
static XrCompileResources *edge_reservation_sampling;
static void edge_reservation_observe(void *pointer) {
    (void)pointer;CHECK(edge_reservation_sampling && !edge_reservation_observed);
    ++edge_reservation_observed;edge_reservation_allocated_work=edge_reservation_sampling->stats.work;
}
static void edge_reservation_rows(const EffectInvocationWorkspace *owner,uint32_t count) {
    CHECK(owner->final.edge_count==count && owner->final.nodes[0].edge_head==count-1 &&
        owner->consumers[1]==count-1);
    for (uint32_t e=0;e<count;++e) {
        const EffectInvocationEdge *row=&owner->final.edges[e];
        CHECK(row->caller==0 && row->target==1 && row->instruction==e && row->producer==UINT32_MAX &&
            !row->latent && row->next==(e?e-1:UINT32_MAX) && row->consumer_next==(e?e-1:UINT32_MAX));
    }
}

/* These are actual indexed edges and consumer links. Numeric control data
 * tests receiving only; original genuine Checked and seal cases continue to
 * supply every semantic admission, producer and source-coordinate oracle. */
static XrXirStatus edge_reservation_run(const XrXirCompileContext *context,unsigned mode,uint32_t expected) {
    EffectInvocationNode nodes[2]={{.instruction_count=32,.edge_head=UINT32_MAX},
        {.instruction_count=32,.edge_head=UINT32_MAX}};
    uint32_t consumers[2]={UINT32_MAX,UINT32_MAX};
    EffectInvocationWorkspace owner={.work=context,.consumers=consumers,.maximum=64,
        .final={.nodes=nodes,.count=2,.capacity=2}};
    XrXirStatus status=XR_XIR_OK;
    for (uint32_t i=0;i<8 && status==XR_XIR_OK;++i)
        status=effect_invocation_edge(&owner,0,1,i,UINT32_MAX,false);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.final.edge_capacity==8);edge_reservation_rows(&owner,8);
    /* Complete this real index growth before measuring vector headroom. The
     * ordinary edge path repeats its original reserve check without growth. */
    status=effect_invocation_edge_index_reserve(&owner);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.edge_index_capacity==32);edge_reservation_prefix_live=rp_stats(context).live_bytes;
    RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(context).live_bytes;
    EffectInvocationEdge *old=owner.final.edges;size_t attempts=rp_attempts;
    bool sample=mode==0 || mode==1 || mode==4;
    if (sample) {
        CHECK(!rp_allocation_observer);edge_reservation_sampling=context->resources;edge_reservation_observed=0;
        rp_allocation_observer=edge_reservation_observe;
    }
    if (mode==2) { rp_fail_at=rp_attempts;rp_injected=false; }
    status=effect_invocation_edge(&owner,0,1,8,UINT32_MAX,false);
    if (sample) { rp_allocation_observer=NULL;edge_reservation_sampling=NULL; }
    if (mode==2) {
        rp_fail_at=SIZE_MAX;CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==attempts+1 &&
            owner.final.edges==old && owner.final.edge_capacity==8 && rp_stats(context).live_bytes==held_live);
        edge_reservation_rows(&owner,8);rp_balanced(held);
        status=effect_invocation_edge(&owner,0,1,8,UINT32_MAX,false);
    }
    if (mode==3) {
        CHECK(status==XR_XIR_BUDGET && rp_attempts==attempts && owner.final.edges==old &&
            owner.final.edge_capacity==8 && rp_stats(context).live_bytes==held_live);
        edge_reservation_rows(&owner,8);rp_balanced(held);
    }
    if (mode==4) {
        CHECK(status==XR_XIR_BUDGET && edge_reservation_observed==1 && rp_attempts==attempts+1 &&
            owner.final.edge_capacity==expected);
        edge_reservation_rows(&owner,8);size_t retry=rp_attempts;
        CHECK(effect_invocation_edge(&owner,0,1,8,UINT32_MAX,false)==XR_XIR_BUDGET && rp_attempts==retry);
        edge_reservation_rows(&owner,8);
    }
    if (status==XR_XIR_OK) {
        CHECK(owner.final.edge_capacity==expected);edge_reservation_rows(&owner,9);
        if (sample) CHECK(edge_reservation_observed==1);
    }
done:
    xr_compile_resources_free(owner.edge_index);xr_compile_resources_free(owner.final.edges);return status;
}

static void invocation_edge_reservation_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(edge_reservation_run(&context,0,12)==XR_XIR_OK);uint64_t prefix_live=edge_reservation_prefix_live;
    rp_owner_free(&context,baseline);rp_balanced(physical);
    uint64_t minimum_live=prefix_live+sizeof(CompileAllocation)+9*sizeof(EffectInvocationEdge);
    XrCompileResourceLimits bounded=rp_caps();CHECK(minimum_live<bounded.live_bytes);bounded.live_bytes=minimum_live;
    physical=rp_mark();context=rp_owner(bounded);baseline=rp_stats(&context).live_bytes;
    CHECK(edge_reservation_run(&context,1,9)==XR_XIR_OK);uint64_t receiving_work=edge_reservation_allocated_work;
    XrCompileResourceStats measured=rp_stats(&context);CHECK(measured.peak_bytes==minimum_live);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();context=rp_owner(bounded);baseline=rp_stats(&context).live_bytes;
    CHECK(edge_reservation_run(&context,2,9)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    XrCompileResourceLimits too_small=bounded;--too_small.live_bytes;
    physical=rp_mark();context=rp_owner(too_small);baseline=rp_stats(&context).live_bytes;
    CHECK(edge_reservation_run(&context,3,0)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);
    XrCompileResourceLimits work=bounded;work.work=receiving_work+8*sizeof(EffectInvocationEdge);
    physical=rp_mark();context=rp_owner(work);baseline=rp_stats(&context).live_bytes;
    CHECK(edge_reservation_run(&context,4,9)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();XrCompileResourceLimits caps=bounded;
        if (!axis) caps.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(edge_reservation_run(&context,5,9)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Edge reservation: original preferred12 and exact finite minimum9, all9 complete key/consumer links, header%zu, minimum-minusone before any allocation, one actual OOM no implicit retry and same finite-owner success, one actual admitted allocation then copy/insertion work refusal and same exhausted-owner retry, six axes%llu/%llu/%llu exact/minusone PASS\n",
        sizeof(CompileAllocation),(unsigned long long)measured.allocated_bytes,
        (unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_EDGE_RESERVATION_CASES_H
