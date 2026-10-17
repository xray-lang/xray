/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_new_child_front_cases.h - One bounded ring for new equations
 */
#ifndef XIR_INVOCATION_NEW_CHILD_FRONT_CASES_H
#define XIR_INVOCATION_NEW_CHILD_FRONT_CASES_H

/* This data-only case checks the actual ring allocation, wrap and growth.
 * Original genuine Checked producer and independent seal cases remain the
 * semantic oracle; numeric node IDs carry no admission conclusion. */
static XrXirStatus new_child_front_operation(const XrXirCompileContext *context) {
    EffectGraph graph={0};EffectInvocationWorkspace owner={.work=context,.graph=&graph,.maximum=32};
    XrXirStatus status=effect_invocation_capacity(&owner);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.final.capacity==8 && !owner.pending && !owner.front && !owner.back);
    if (!xir_compile_work(context,8*sizeof(*owner.final.nodes))) { status=XR_XIR_BUDGET;goto done; }
    for (uint32_t n=0;n<8;++n) owner.final.nodes[n]=(EffectInvocationNode){0};
    owner.final.count=8;
    for (uint32_t n=0;n<8 && status==XR_XIR_OK;++n)
        status=n%2?effect_invocation_enqueue(&owner,n):effect_invocation_enqueue_first(&owner,n);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.pending==8);
    uint32_t front=owner.front,back=owner.back;
    status=effect_invocation_enqueue_first(&owner,5);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.pending==8 && owner.front==front && owner.back==back);
    CHECK(effect_invocation_enqueue_first(&owner,8)==XR_XIR_BAD_STRUCTURE);
    queued_child_pop(&owner,6);queued_child_pop(&owner,4);
    status=effect_invocation_enqueue_first(&owner,4);
    if (status==XR_XIR_OK) status=effect_invocation_enqueue(&owner,6);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.pending==8 && owner.front==owner.back && owner.front!=0);
    status=effect_invocation_capacity(&owner);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.final.capacity==12 && owner.pending==8 && owner.front==0 && owner.back==8);
    const uint32_t before[8]={4,2,0,1,3,5,7,6};
    for (uint32_t q=0;q<8;++q) CHECK(graph.queue[q]==before[q] && owner.states[before[q]].queued);
    if (!xir_compile_work(context,4*sizeof(*owner.final.nodes))) { status=XR_XIR_BUDGET;goto done; }
    for (uint32_t n=8;n<12;++n) owner.final.nodes[n]=(EffectInvocationNode){0};
    owner.final.count=12;
    for (uint32_t n=8;n<12 && status==XR_XIR_OK;++n)
        status=n%2?effect_invocation_enqueue(&owner,n):effect_invocation_enqueue_first(&owner,n);
    if (status!=XR_XIR_OK) goto done;
    CHECK(owner.pending==12 && owner.front==owner.back);
    const uint32_t after[12]={10,8,4,2,0,1,3,5,7,6,9,11};
    for (uint32_t q=0;q<12;++q) queued_child_pop(&owner,after[q]);
    CHECK(!owner.pending && owner.front==owner.back);
    for (uint32_t n=0;n<12;++n) CHECK(!owner.states[n].queued);
done:
    xr_compile_resources_free(owner.final.nodes);xr_compile_resources_free(owner.consumers);
    xr_compile_resources_free(graph.queue);return status;
}

static void invocation_new_child_front_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;CHECK(!rp_allocation_observer);
    queued_child_sampling=context.resources;queued_child_allocation_count=0;rp_allocation_observer=queued_child_allocation_observe;
    XrXirStatus status=new_child_front_operation(&context);rp_allocation_observer=NULL;queued_child_sampling=NULL;
    CHECK(status==XR_XIR_OK);size_t sites=queued_child_allocation_count;CHECK(sites==6);
    XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (size_t at=0;at<sites;++at) {
        physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark held=rp_mark();XrCompileResources *identity=context.resources;
        rp_fail_at=rp_attempts+at;rp_injected=false;status=new_child_front_operation(&context);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && context.resources==identity);rp_balanced(held);
        CHECK(new_child_front_operation(&context)==XR_XIR_OK && context.resources==identity);rp_balanced(held);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (size_t at=0;at<sites;++at) {
        physical=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=queued_child_allocation_work[at];
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;RootParameterMark held=rp_mark();
        CHECK(new_child_front_operation(&context)==XR_XIR_BUDGET);rp_balanced(held);
        CHECK(new_child_front_operation(&context)==XR_XIR_BUDGET);rp_balanced(held);
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if (!axis) caps.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(new_child_front_operation(&context)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"New child priority: original FIFO notifications and one bounded ring, full wrap and actual growth8to12 preserve complete ordered12-node oracle, duplicate and bad-node controls, all%zu actual OOM/work cuts and same finite-owner retry, six axes%llu/%llu/%llu exact/minusone PASS\n",
        sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_NEW_CHILD_FRONT_CASES_H
