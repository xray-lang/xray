/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_node_occupancy_cases.h - Dense complete physical input keys
 */
#ifndef XIR_INVOCATION_NODE_OCCUPANCY_CASES_H
#define XIR_INVOCATION_NODE_OCCUPANCY_CASES_H
static uint64_t node_occupancy_receiving_work;
static size_t node_occupancy_observed;
static XrCompileResources *node_occupancy_sampling;
static void node_occupancy_observe(void *pointer) {
    (void)pointer;CHECK(node_occupancy_sampling && !node_occupancy_observed);
    ++node_occupancy_observed;node_occupancy_receiving_work=node_occupancy_sampling->stats.work;
}

/* Hashes are deliberately held constant for the absent-key probes. Every
 * current input word and structural dimension must still decide equality;
 * an instruction-span mismatch remains an inconsistency, not a new key. */
static XrXirStatus node_occupancy_verify(EffectInvocationWorkspace *owner,const uint64_t *oracle) {
    CHECK(owner->final.count==24);size_t attempts=rp_attempts;
    for (uint32_t n=0;n<24;++n) {
        CHECK(!memcmp(owner->final.nodes[n].input,oracle+(size_t)n*4,4*sizeof(uint64_t)));
        EffectInvocationNodeKey key={0};XrXirStatus status=effect_invocation_node_existing_key(owner,n,&key);
        if (status!=XR_XIR_OK) return status;
        CHECK(key.root==1 && key.body==1 && key.parameters==2 && key.instructions==2 && key.words==4);
        size_t at=SIZE_MAX;status=effect_invocation_node_slot(owner,owner->node_index,owner->node_index_capacity,&key,&at);
        if (status!=XR_XIR_OK) return status;
        CHECK(owner->node_index[at]==n+1);
        for (uint32_t word=0;word<4;++word) {
            uint64_t changed[4];memcpy(changed,oracle+(size_t)n*4,sizeof(changed));changed[word]^=UINT64_C(1)<<40;
            EffectInvocationNodeKey absent=key;absent.input=changed;size_t position=SIZE_MAX;
            status=effect_invocation_node_slot(owner,owner->node_index,owner->node_index_capacity,&absent,&position);
            if (status!=XR_XIR_OK) return status;
            CHECK(!owner->node_index[position]);
        }
        for (unsigned field=0;field<3;++field) {
            EffectInvocationNodeKey absent=key;
            if (!field) absent.root+=8;else if (field==1) absent.body+=8;else ++absent.parameters;
            size_t position=SIZE_MAX;
            status=effect_invocation_node_slot(owner,owner->node_index,owner->node_index_capacity,&absent,&position);
            if (status!=XR_XIR_OK) return status;
            CHECK(!owner->node_index[position]);
        }
        EffectInvocationNodeKey span=key;++span.instructions;size_t output=SIZE_MAX;
        status=effect_invocation_node_slot(owner,owner->node_index,owner->node_index_capacity,&span,&output);
        if (status==XR_XIR_BUDGET) return status;
        CHECK(status==XR_XIR_BAD_STRUCTURE && output==SIZE_MAX);
        uint32_t saved=owner->node_index[at];owner->node_index[at]=owner->final.count+1;output=SIZE_MAX;
        status=effect_invocation_node_slot(owner,owner->node_index,owner->node_index_capacity,&key,&output);
        owner->node_index[at]=saved;
        if (status==XR_XIR_BUDGET) return status;
        CHECK(status==XR_XIR_BAD_STRUCTURE && output==SIZE_MAX);
    }
    CHECK(rp_attempts==attempts);return XR_XIR_OK;
}

/* Genuine immutable heads determine the complete row width. Synthetic
 * four-word keys test index data only and never enter semantic publication. */
static XrXirStatus node_occupancy_run(const XrXirCompileContext *context,unsigned mode) {
    GenerationRangeTrial trial;generation_range_fixture(&trial,0);
    XrXirStatus status=generation_range_prepare(context,&trial);
    if (status!=XR_XIR_OK) goto done;
    EffectBodyMaterial *material=trial.effects.generation;
    CHECK(material->functions[1].parameter_count==2 && material->functions[1].instruction_count==2);
    EffectInvocationNode nodes[24]={0};uint64_t inputs[24][4],oracle[96];
    for (uint32_t n=0;n<24;++n) {
        for (uint32_t w=0;w<4;++w) inputs[n][w]=oracle[(size_t)n*4+w]=((uint64_t)n+1)*(w+3);
        nodes[n]=(EffectInvocationNode){.root=1,.body=1,.parameter_count=2,.instruction_count=2,.input=inputs[n]};
    }
    EffectInvocationWorkspace owner={.work=context,.body=effect_body_owned_request(context,material),
        .final={.material=material,.nodes=nodes,.capacity=24}};
    owner.node_index=xir_compile_calloc(context,32,sizeof(*owner.node_index),&status);
    if (!owner.node_index) goto done;
    owner.node_index_capacity=32;
    for (uint32_t n=0;n<24 && status==XR_XIR_OK;++n) {
        owner.final.count=n+1;EffectInvocationNodeKey key={0};size_t at=SIZE_MAX;
        status=effect_invocation_node_existing_key(&owner,n,&key);
        if (status==XR_XIR_OK) status=effect_invocation_node_slot(&owner,owner.node_index,32,&key,&at);
        if (status!=XR_XIR_OK) break;
        CHECK(!owner.node_index[at]);
        if (!xir_compile_work(context,sizeof(uint32_t))) { status=XR_XIR_BUDGET;break; }
        owner.node_index[at]=n+1;
        if (n==22) {
            size_t attempts=rp_attempts;uint32_t *old=owner.node_index;
            status=effect_invocation_node_index_reserve(&owner);
            if (status==XR_XIR_OK) CHECK(owner.node_index==old && owner.node_index_capacity==32 && rp_attempts==attempts);
        }
    }
    if (status==XR_XIR_OK) status=node_occupancy_verify(&owner,oracle);
    if (status==XR_XIR_OK) {
        uint32_t before[32];memcpy(before,owner.node_index,sizeof(before));uint32_t *old=owner.node_index;
        RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(context).live_bytes;size_t attempts=rp_attempts;
        if (mode==1 || mode==3) {
            CHECK(!rp_allocation_observer);node_occupancy_sampling=context->resources;node_occupancy_observed=0;
            rp_allocation_observer=node_occupancy_observe;
        }
        if (mode==2) { rp_fail_at=rp_attempts;rp_injected=false; }
        status=effect_invocation_node_index_reserve(&owner);
        if (mode==1 || mode==3) { rp_allocation_observer=NULL;node_occupancy_sampling=NULL; }
        if (mode==2) {
            rp_fail_at=SIZE_MAX;
            CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==attempts+1 &&
                owner.node_index==old && owner.node_index_capacity==32 && !memcmp(old,before,sizeof(before)) &&
                !memcmp(inputs,oracle,sizeof(inputs)) && rp_stats(context).live_bytes==held_live);rp_balanced(held);
            status=effect_invocation_node_index_reserve(&owner);
        }
        if (mode==3) {
            CHECK(status==XR_XIR_BUDGET && node_occupancy_observed==1 && rp_attempts==attempts+1 &&
                owner.node_index==old && owner.node_index_capacity==32 && !memcmp(old,before,sizeof(before)) &&
                !memcmp(inputs,oracle,sizeof(inputs)) && rp_stats(context).live_bytes==held_live);rp_balanced(held);
            size_t retry=rp_attempts;CHECK(effect_invocation_node_index_reserve(&owner)==XR_XIR_BUDGET && rp_attempts==retry);
        }
        if (status==XR_XIR_OK) {
            CHECK(owner.node_index_capacity==64 && owner.node_index!=old && owner.final.count==24);
            if (mode==1) CHECK(node_occupancy_observed==1);
            status=node_occupancy_verify(&owner,oracle);
        }
    }
    xr_compile_resources_free(owner.node_index);
done:
    effect_graph_free(&trial.graph);invocation_core_storage_free(&trial.effects);
    xr_xir_compile_artifact_free(trial.checked);return status;
}

static void invocation_node_occupancy_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;CHECK(node_occupancy_run(&context,1)==XR_XIR_OK);
    XrCompileResourceStats measured=rp_stats(&context);uint64_t receiving=node_occupancy_receiving_work;
    rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(node_occupancy_run(&context,2)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();XrCompileResourceLimits limits=rp_caps();limits.work=receiving+256+64;
    context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
    CHECK(node_occupancy_run(&context,3)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();limits=rp_caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(node_occupancy_run(&context,0)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Node occupancy: genuine immutable heads,24 independent four-word keys in32 then64 slots, every96 input-word and72 dimension forced-hash collisions absent per walk,24 span and24 malformed-bucket refusals preserve output; actual1 receiving OOM/rebuild-work cut and same finite-owner retries keep old index/all96 input words, whole genuine-fixture six axes%llu/%llu/%llu exact/minusone; independent local/global causes unchanged, no numeric semantic admission PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_NODE_OCCUPANCY_CASES_H
