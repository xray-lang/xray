/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_queued_child_cases.h - Real edges and pending first expansion
 */
#ifndef XIR_INVOCATION_QUEUED_CHILD_CASES_H
#define XIR_INVOCATION_QUEUED_CHILD_CASES_H
static XrCompileResources *queued_child_sampling;
static uint64_t queued_child_allocation_work[16];
static size_t queued_child_allocation_count;
static void queued_child_allocation_observe(void *pointer) {
    (void)pointer;CHECK(queued_child_sampling && queued_child_allocation_count<16);
    queued_child_allocation_work[queued_child_allocation_count++]=queued_child_sampling->stats.work;
}
static void queued_child_pop(EffectInvocationWorkspace *owner,uint32_t expected) {
    CHECK(owner->pending && owner->graph->queue[owner->front]==expected && owner->states[expected].queued);
    owner->front=owner->front+1==owner->final.capacity?0:owner->front+1;
    --owner->pending;owner->states[expected].queued=false;
}
/* Numeric control fixtures prove queue order and actual linkage only. The
 * genuine Checked producer cases below carry every semantic admission. */
static XrXirStatus queued_child_operation(const XrXirCompileContext *context,unsigned mode) {
    CHECK(mode<8);EffectInvocationNode nodes[3]={0};EffectInvocationState states[3]={0};uint32_t consumers[3],queue[3]={0};
    for(uint32_t n=0;n<3;++n){nodes[n].root=0;nodes[n].body=n;nodes[n].instruction_count=4;nodes[n].edge_head=UINT32_MAX;consumers[n]=UINT32_MAX;}
    EffectGraph graph={.queue=queue};EffectInvocationWorkspace owner={.work=context,.graph=&graph,.states=states,.consumers=consumers,
        .maximum=16,.producer_enabled=mode!=5 && mode!=6,.class_enabled=mode==6,
        .final={.nodes=nodes,.count=3,.capacity=3}};
    uint32_t target=mode==3?0:1;bool waiting=mode==0 || mode==4 || mode==6 || mode==7;
    if(waiting){states[target].queued=true;queue[0]=target;owner.pending=1;owner.back=1;}
    states[target].expanded=mode==1 || mode==4;bool latent=mode==7;
    XrXirStatus status=effect_invocation_edge(&owner,0,target,0,UINT32_MAX,latent);
    if(status==XR_XIR_OK){bool wake=mode>=1 && mode<=4;
        CHECK(owner.final.edge_count==1 && nodes[0].edge_head==0 && consumers[target]==0 &&
            owner.final.edges[0].caller==0 && owner.final.edges[0].target==target &&
            owner.final.edges[0].instruction==0 && owner.final.edges[0].latent==latent &&
            owner.final.edges[0].next==UINT32_MAX && owner.final.edges[0].consumer_next==UINT32_MAX);
        CHECK(states[0].queued==wake && owner.pending==(uint32_t)waiting+(uint32_t)wake);
        uint32_t pending=owner.pending,front=owner.front,back=owner.back;
        status=effect_invocation_edge(&owner,0,target,0,UINT32_MAX,latent);
        if(status==XR_XIR_OK){CHECK(owner.final.edge_count==1 && owner.pending==pending && owner.front==front && owner.back==back);
            if(waiting){queued_child_pop(&owner,target);states[target].expanded=true;}
            if(wake)queued_child_pop(&owner,0);
            if(mode==0 || mode==6 || mode==7){
                /* This is the same original notification primitive and actual
                 * consumer chain used after a child's result changes. */
                for(uint32_t e=consumers[target];e!=UINT32_MAX;e=owner.final.edges[e].consumer_next){
                    CHECK(owner.final.edges[e].target==target);status=effect_invocation_enqueue(&owner,owner.final.edges[e].caller);
                    if(status!=XR_XIR_OK)break;}
                if(status==XR_XIR_OK){CHECK(states[0].queued && owner.pending==1);queued_child_pop(&owner,0);}
            }
            if(status==XR_XIR_OK)CHECK(!owner.pending && !states[0].queued);
        }
    }
    xr_compile_resources_free(owner.edge_index);xr_compile_resources_free(owner.final.edges);return status;
}
static void invocation_queued_child_returned_controls(void) {
    for(uint32_t mode=0;mode<8;++mode){RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;fprintf(stderr,"Queued child genuine returned producer mode%u\n",mode);
        CHECK(conditional_invocation_returned(&context,mode,true)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);}
}
static void invocation_queued_child_controls(void) {
    for(unsigned mode=0;mode<8;++mode){RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;CHECK(queued_child_operation(&context,mode)==XR_XIR_OK);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(!rp_allocation_observer);queued_child_sampling=context.resources;queued_child_allocation_count=0;
    rp_allocation_observer=queued_child_allocation_observe;XrXirStatus status=queued_child_operation(&context,0);
    rp_allocation_observer=NULL;queued_child_sampling=NULL;CHECK(status==XR_XIR_OK);
    size_t sites=queued_child_allocation_count;CHECK(sites);XrCompileResourceStats measured=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for(size_t at=0;at<sites;++at){physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        RootParameterMark held=rp_mark();XrCompileResources *identity=context.resources;
        rp_fail_at=rp_attempts+at;rp_injected=false;status=queued_child_operation(&context,0);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && context.resources==identity);rp_balanced(held);
        CHECK(queued_child_operation(&context,0)==XR_XIR_OK && context.resources==identity);rp_balanced(held);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(size_t at=0;at<sites;++at){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=queued_child_allocation_work[at];
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;RootParameterMark held=rp_mark();
        CHECK(queued_child_operation(&context,0)==XR_XIR_BUDGET);rp_balanced(held);
        CHECK(queued_child_operation(&context,0)==XR_XIR_BUDGET);rp_balanced(held);rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(queued_child_operation(&context,0)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);}
    invocation_queued_child_returned_controls();
    fprintf(stderr,"Queued first-expansion child: eight real edge/control cases preserve consumer chains, changed-child notification, completed/active/self/latent/duplicate behavior; actual%zu OOM/work cuts and same finite-owner retry, six axes%llu/%llu/%llu exact/minusone; genuine Checked returned producer all8 modes including relay/UNKNOWN/WEAKEN/unrooted recursion/Cell capture and independent seal/owner deaths/lowering PASS\n",
        sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_QUEUED_CHILD_CASES_H
