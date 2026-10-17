/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_node_quarter_cases.h - Complete vector and wrapped queue copies
 */
#ifndef XIR_INVOCATION_NODE_QUARTER_CASES_H
#define XIR_INVOCATION_NODE_QUARTER_CASES_H
static XrCompileResources *node_quarter_sampling;
static uint64_t node_quarter_allocation_work[3];
static size_t node_quarter_allocation_count;
static void node_quarter_observe(void *pointer) {
    (void)pointer;CHECK(node_quarter_sampling && node_quarter_allocation_count<3);
    node_quarter_allocation_work[node_quarter_allocation_count++]=node_quarter_sampling->stats.work;
}

/* Every byte belongs to these actual data vectors. Numeric fields test copy
 * integrity only; genuine Checked and independent seal cases prove semantics. */
static XrXirStatus node_quarter_run(const XrXirCompileContext *context,unsigned mode,size_t site) {
    EffectGraph graph={0};EffectInvocationWorkspace owner={.work=context,.graph=&graph,.maximum=64};
    uint64_t words[18][2];XrXirStatus status=XR_XIR_OK;
    const uint32_t initial[3]={8,12,18};
    for (unsigned growth=0;growth<3 && status==XR_XIR_OK;++growth) {
        uint32_t old=owner.final.count;status=effect_invocation_capacity(&owner);
        if (status!=XR_XIR_OK) break;
        CHECK(owner.final.capacity==initial[growth]);
        if (!xir_compile_work(context,(uint64_t)(owner.final.capacity-old)*sizeof(EffectInvocationNode))) {
            status=XR_XIR_BUDGET;break;
        }
        for (uint32_t n=old;n<owner.final.capacity;++n) {
            words[n][0]=n*19+3;words[n][1]=n*23+7;
            owner.final.nodes[n]=(EffectInvocationNode){.root=n,.body=n*2,.parameter_count=n%3,
                .instruction_count=n+5,.intrinsic_mask=n%4,.local_mask=n%2,.input=words[n],
                .return_coverage=UINT64_C(1)<<n,.class_return_rank=n*17,.edge_head=100+n,
                .deferred=!!(n%2),.local_deferred=!!(n%3)};
            owner.consumers[n]=1000+n;owner.states[n].expanded=!!(n%2);
        }
        owner.final.count=owner.final.capacity;
    }
    for (uint32_t n=0;n<18 && status==XR_XIR_OK;++n) status=effect_invocation_enqueue(&owner,n);
    if (status==XR_XIR_OK) {
        queued_child_pop(&owner,0);queued_child_pop(&owner,1);
        status=effect_invocation_enqueue_first(&owner,1);
        if (status==XR_XIR_OK) status=effect_invocation_enqueue(&owner,0);
    }
    if (status==XR_XIR_OK) {
        CHECK(owner.pending==18 && owner.front==owner.back && owner.front!=0);
        EffectInvocationNode oracle[18];uint32_t consumers[18],queue[18];EffectInvocationState states[18];
        memcpy(oracle,owner.final.nodes,sizeof(oracle));memcpy(consumers,owner.consumers,sizeof(consumers));
        memcpy(states,owner.states,sizeof(states));memcpy(queue,graph.queue,sizeof(queue));
        EffectInvocationNode *old_nodes=owner.final.nodes;uint32_t *old_consumers=owner.consumers,*old_queue=graph.queue;
        EffectInvocationState *old_states=owner.states;uint32_t front=owner.front,back=owner.back;
        RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(context).live_bytes;size_t attempts=rp_attempts;
        if (mode==1) {
            CHECK(!rp_allocation_observer);node_quarter_sampling=context->resources;node_quarter_allocation_count=0;
            rp_allocation_observer=node_quarter_observe;
        }
        if (mode==2) { CHECK(site<3);rp_fail_at=rp_attempts+site;rp_injected=false; }
        if (mode==4) owner.maximum=19;
        status=effect_invocation_capacity(&owner);
        if (mode==1) { rp_allocation_observer=NULL;node_quarter_sampling=NULL;CHECK(node_quarter_allocation_count==3); }
        if (mode==2) rp_fail_at=SIZE_MAX;
        if (mode==2 || mode==3) {
            CHECK(status==(mode==2?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET) && rp_attempts==attempts+site+1);
            CHECK(mode!=2 || rp_injected);
            CHECK(owner.final.nodes==old_nodes && owner.consumers==old_consumers && owner.states==old_states &&
                graph.queue==old_queue && owner.final.capacity==18 && owner.final.count==18 && owner.pending==18 &&
                owner.front==front && owner.back==back && !memcmp(old_nodes,oracle,sizeof(oracle)) &&
                !memcmp(old_consumers,consumers,sizeof(consumers)) && !memcmp(old_states,states,sizeof(states)) &&
                !memcmp(old_queue,queue,sizeof(queue)) && rp_stats(context).live_bytes==held_live);rp_balanced(held);
            if (mode==2) status=effect_invocation_capacity(&owner);
            else {
                size_t retry=rp_attempts;CHECK(effect_invocation_capacity(&owner)==XR_XIR_BUDGET && rp_attempts==retry);
            }
        }
        if (status==XR_XIR_OK) {
            uint32_t capacity=mode==4?19u:22u;
            CHECK(owner.final.capacity==capacity && owner.final.count==18 && owner.pending==18 && owner.front==0 && owner.back==18);
            CHECK(owner.final.nodes!=old_nodes && owner.consumers!=old_consumers && graph.queue!=old_queue);
            CHECK(!memcmp(owner.final.nodes,oracle,sizeof(oracle)) && !memcmp(owner.consumers,consumers,sizeof(consumers)) &&
                !memcmp(owner.states,states,sizeof(states)));
            for (uint32_t n=0;n<18;++n) CHECK(graph.queue[n]==(n+1)%18);
            for (uint32_t n=18;n<capacity;++n) CHECK(owner.consumers[n]==UINT32_MAX && !owner.states[n].queued && !owner.states[n].expanded);
            for (uint32_t n=0;n<18;++n) queued_child_pop(&owner,(n+1)%18);
            CHECK(!owner.pending && owner.front==owner.back);
        }
    }
    xr_compile_resources_free(owner.final.nodes);xr_compile_resources_free(owner.consumers);xr_compile_resources_free(graph.queue);
    return status;
}

static void invocation_node_quarter_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;CHECK(node_quarter_run(&context,1,0)==XR_XIR_OK);
    XrCompileResourceStats measured=rp_stats(&context);uint64_t cuts[3];memcpy(cuts,node_quarter_allocation_work,sizeof(cuts));
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for (size_t at=0;at<3;++at) {
        physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        CHECK(node_quarter_run(&context,2,at)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();limits.work=cuts[at];
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(node_quarter_run(&context,3,at)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(node_quarter_run(&context,4,0)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(node_quarter_run(&context,0,0)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Node spare: original8to12to18 then actual18to22 and maximum19, complete18 node bytes/consumer/state and wrapped queue oracle, all3 actual receiving OOM/admitted-allocation work cuts and same finite-owner retries preserve old three buffers, six axes%llu/%llu/%llu exact/minusone; no numeric semantic admission PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_NODE_QUARTER_CASES_H
