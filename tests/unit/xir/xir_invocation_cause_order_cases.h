/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_cause_order_cases.h - Retired equation scratch and complete root order
 *
 * KEY CONCEPT:
 *   A live exact-key index cannot be overwritten. Its retired allocation holds
 *   node numbers only and must enumerate the complete original inventory.
 */
#ifndef XIR_INVOCATION_CAUSE_ORDER_CASES_H
#define XIR_INVOCATION_CAUSE_ORDER_CASES_H

/* This storage fixture confers no permission. Real admission and cause
 * witnesses remain checked by the retained public effects tests. */
static void cause_order_prepare(const XrXirCompileContext *context,
    EffectInvocationWorkspace *owner,EffectInvocationNode *nodes,
    EffectInvocationState *states,uint32_t *roots) {
    for(uint32_t root=0;root<4;++root) roots[root]=root;
    for(uint32_t n=0;n<6;++n)
        nodes[n]=(EffectInvocationNode){.root=n%3,.body=(n*3+1)%4};
    memset(states,0,6*sizeof(*states));
    *owner=(EffectInvocationWorkspace){.read={.work=context,.final=&owner->final,
        .body={.domain={.function_count=4}}},.states=states,
        .final={.nodes=nodes,.count=6,.capacity=6,.root_spaces=roots}};
    CHECK(effect_invocation_node_index_reserve(owner)==XR_XIR_OK && owner->node_index_capacity==4);
}

static void cause_order_scalar(const EffectInvocationWorkspace *owner,const uint32_t *groups) {
    uint32_t visited=0;
    for(uint32_t root=0;root<4;++root) {
        uint32_t entry=groups[root];
        for(uint32_t n=0;n<6;++n) {
            if(owner->final.nodes[n].root!=root) continue;
            CHECK(entry==n+1 && !(visited&(UINT32_C(1)<<n)));
            visited|=UINT32_C(1)<<n;entry=owner->states[n].lookup_next;
        }
        CHECK(!entry);
    }
    CHECK(visited==UINT32_C(63));
}

static uint64_t cause_order_lifetime(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    EffectInvocationNode nodes[6];EffectInvocationState states[6];uint32_t roots[4];
    EffectInvocationWorkspace owner;cause_order_prepare(&context,&owner,nodes,states,roots);
    uint32_t *groups=owner.node_index,slots[4];EffectInvocationState held[6];
    memcpy(slots,groups,sizeof(slots));memcpy(held,states,sizeof(held));
    XrCompileResourceStats live=rp_stats(&context);
    CHECK(effect_invocation_cause_groups(&owner,groups)==XR_XIR_BAD_STRUCTURE &&
        rp_stats(&context).work==live.work && !memcmp(slots,groups,sizeof(slots)) &&
        !memcmp(held,states,sizeof(held)));
    for(uint32_t n=0;n<6;++n) {
        EffectInvocationNodeLookup key={nodes[n].root,nodes[n].body,0,NULL,0};
        uint32_t actual=UINT32_MAX;
        CHECK(effect_invocation_node_find(&owner,&key,&actual)==XR_XIR_OK && actual==n);
    }
    owner.node_index=NULL;owner.node_index_capacity=0;
    EffectInvocationNodeLookup key={nodes[0].root,nodes[0].body,0,NULL,0};uint32_t output=42;
    CHECK(effect_invocation_node_find(&owner,&key,&output)==XR_XIR_BAD_STRUCTURE && output==42);
    XrCompileResourceStats before=rp_stats(&context);size_t attempts=rp_attempts;
    CHECK(effect_invocation_cause_groups(&owner,groups)==XR_XIR_OK);cause_order_scalar(&owner,groups);
    XrCompileResourceStats after=rp_stats(&context);uint64_t measured=after.work-before.work;
    CHECK(measured && after.allocated_bytes==before.allocated_bytes &&
        after.live_bytes==before.live_bytes && after.peak_bytes==before.peak_bytes &&
        after.allocation_count==before.allocation_count && rp_attempts==attempts);
    nodes[5].root=4;CHECK(effect_invocation_cause_groups(&owner,groups)==XR_XIR_BAD_STRUCTURE);
    nodes[5].root=2;roots[2]=0;
    CHECK(effect_invocation_cause_groups(&owner,groups)==XR_XIR_BAD_STRUCTURE);roots[2]=2;
    states[5].queued=true;
    CHECK(effect_invocation_cause_groups(&owner,groups)==XR_XIR_BAD_STRUCTURE);states[5].queued=false;
    CHECK(effect_invocation_cause_groups(&owner,groups)==XR_XIR_OK);cause_order_scalar(&owner,groups);
    CHECK(!owner.node_index && !owner.node_index_capacity && rp_attempts==attempts &&
        rp_stats(&context).work>after.work && rp_stats(&context).allocated_bytes==after.allocated_bytes);
    xr_compile_resources_free(groups);rp_owner_free(&context,baseline);rp_balanced(physical);return measured;
}

static void cause_order_boundaries(uint64_t measured) {
    for(uint32_t minus=0;minus<2;++minus) {
        RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;
        EffectInvocationNode nodes[6];EffectInvocationState states[6];uint32_t roots[4];
        EffectInvocationWorkspace owner;cause_order_prepare(&context,&owner,nodes,states,roots);
        uint32_t *groups=owner.node_index;owner.node_index=NULL;owner.node_index_capacity=0;
        XrCompileResourceStats start=rp_stats(&context);uint64_t remaining=measured-minus;
        CHECK(start.work+remaining<=rp_caps().work && xr_compile_resources_work(context.resources,
            rp_caps().work-start.work-remaining)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats before=rp_stats(&context);size_t attempts=rp_attempts;
        CHECK(effect_invocation_cause_groups(&owner,groups)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        XrCompileResourceStats after=rp_stats(&context);
        CHECK(after.work>=before.work && after.work<=rp_caps().work &&
            after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes &&
            after.allocation_count==before.allocation_count && rp_attempts==attempts &&
            !owner.node_index && !owner.node_index_capacity);
        if(!minus) {CHECK(after.work-before.work==measured);cause_order_scalar(&owner,groups);}
        else CHECK(effect_invocation_cause_groups(&owner,groups)==XR_XIR_BUDGET &&
            rp_stats(&context).work==after.work);
        xr_compile_resources_free(groups);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
}

static void invocation_cause_order_cases(void) {
    uint64_t measured=cause_order_lifetime();cause_order_boundaries(measured);
    puts("CAUSE_ORDER live index refusal, retired storage, complete scalar order, exact/minus1, physical0 PASS");
}
#endif // XIR_INVOCATION_CAUSE_ORDER_CASES_H
