/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_cause_directory_cases.h - Complete root groups and path data
 */
#ifndef XIR_INVOCATION_CAUSE_DIRECTORY_CASES_H
#define XIR_INVOCATION_CAUSE_DIRECTORY_CASES_H
static uint64_t cause_directory_allocation_work;
static size_t cause_directory_allocations;
static XrCompileResources *cause_directory_sampling;
static void cause_directory_observe(void *pointer) {
    (void)pointer;CHECK(cause_directory_sampling && !cause_directory_allocations);
    ++cause_directory_allocations;cause_directory_allocation_work=cause_directory_sampling->stats.work;
}

/* A genuine immutable material supplies current heads and CALL shapes. The
 * synthetic interleaved nodes below test directory and path data only; they
 * are never published as semantic evidence or a permission certificate. */
static XrXirStatus cause_directory_run(const XrXirCompileContext *context,unsigned mode) {
    GenerationRangeTrial trial;generation_range_fixture(&trial,0);
    XrXirStatus status=generation_range_prepare(context,&trial);
    if (status!=XR_XIR_OK) goto done;
    EffectBodyMaterial *material=trial.effects.generation;CHECK(effect_body_count(material)==4);
    CHECK(!material->functions[0].parameter_count && !material->functions[3].parameter_count);
    EffectInvocationNode nodes[6]={0};EffectInvocationCause causes[6][3]={0};
    EffectInvocationState states[6]={0};EffectInvocationEdge edges[6]={0};
    uint32_t queue[6]={0},consumers[6]={0},root_spaces[4]={0,1,2,3};EffectGraph graph={.queue=queue};
    EffectInvocationWorkspace owner={.work=context,.graph=&graph,.states=states,.consumers=consumers,
        .body=effect_body_owned_request(context,material),
        .final={.material=material,.nodes=nodes,.count=6,.capacity=6,.root_spaces=root_spaces,
            .edges=edges,.edge_count=6,.edge_capacity=6}};
    for (uint32_t n=0;n<6;++n) {
        nodes[n]=(EffectInvocationNode){.root=n%2?3u:0u,.body=0,.intrinsic_mask=XR_XIR_CALLABLE_ROOT_REQUIRED,
            .local_mask=n>=4?XR_XIR_CALLABLE_ROOT_REQUIRED:0,.causes=causes[n],.cause_count=3};
        for (uint32_t a=0;a<3;++a)
            causes[n][a]=(EffectInvocationCause){.next=UINT32_MAX,.edge=UINT32_MAX,.distance=UINT32_MAX};
        if (n>=4) causes[n][0]=(EffectInvocationCause){
            .witness={XR_XIR_ROOT_CAUSE_INITIALIZER,UINT32_MAX,UINT32_MAX,UINT32_MAX,0},
            .next=UINT32_MAX,.edge=UINT32_MAX,.distance=0};
        uint32_t target=n<4?n+2:n-4;
        edges[n]=(EffectInvocationEdge){.caller=n,.target=target,.instruction=4,.producer=UINT32_MAX,
            .next=UINT32_MAX,.consumer_next=UINT32_MAX};consumers[target]=n;
    }
    if (mode==4) nodes[0].root=4;
    RootParameterMark held=rp_mark();uint64_t held_live=rp_stats(context).live_bytes;
    if (mode==1) {
        CHECK(!rp_allocation_observer);cause_directory_sampling=context->resources;cause_directory_allocations=0;
        rp_allocation_observer=cause_directory_observe;
    }
    if (mode==2) { rp_fail_at=rp_attempts;rp_injected=false; }
    status=effect_invocation_causes(&owner);
    if (mode==1) { rp_allocation_observer=NULL;cause_directory_sampling=NULL; }
    if (mode==2) {
        rp_fail_at=SIZE_MAX;CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected);
        rp_balanced(held);CHECK(rp_stats(context).live_bytes==held_live);
        status=effect_invocation_causes(&owner);
    }
    if (mode==3 && status==XR_XIR_BUDGET) {
        rp_balanced(held);CHECK(effect_invocation_causes(&owner)==XR_XIR_BUDGET);
    }
    if (mode==4) CHECK(status==XR_XIR_BAD_STRUCTURE);
    if (status==XR_XIR_OK) for (uint32_t n=0;n<6;++n) {
        uint32_t distance=2-n/2;CHECK(!states[n].queued && causes[n][0].distance==distance &&
            causes[n][0].witness.distance==distance && causes[n][0].witness.cause==
                (distance?XR_XIR_ROOT_CAUSE_CALL:XR_XIR_ROOT_CAUSE_INITIALIZER));
        CHECK(causes[n][0].next==(distance?n+2:UINT32_MAX) &&
            causes[n][0].edge==(distance?n:UINT32_MAX) &&
            causes[n][0].witness.instruction==(distance?4u:UINT32_MAX));
        for (uint32_t a=1;a<3;++a) CHECK(!causes[n][a].witness.cause && causes[n][a].distance==UINT32_MAX &&
            causes[n][a].next==UINT32_MAX && causes[n][a].edge==UINT32_MAX);
    }
    rp_balanced(held);CHECK(rp_stats(context).live_bytes==held_live);
done:
    effect_graph_free(&trial.graph);invocation_core_storage_free(&trial.effects);
    xr_xir_compile_artifact_free(trial.checked);return status;
}

static void invocation_cause_directory_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(cause_directory_run(&context,1)==XR_XIR_OK && cause_directory_allocations==1);
    XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(cause_directory_run(&context,2)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=cause_directory_allocation_work;
    context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
    CHECK(cause_directory_run(&context,3)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(cause_directory_run(&context,4)==XR_XIR_BAD_STRUCTURE);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();caps=rp_caps();
        if (!axis) caps.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(cause_directory_run(&context,0)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Cause directory: genuine current material and interleaved six-node two-cycle data, complete18 cause/distance/edge/witness oracle and bad root, actual1 directory OOM/work cut and same finite-owner retry, whole genuine-fixture six axes%llu/%llu/%llu exact/minusone; no numeric semantic admission PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_CAUSE_DIRECTORY_CASES_H
