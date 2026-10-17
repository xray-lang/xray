/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_driver_lookup_lifetime_cases.h - Private lookup lifetime and seal retry
 */
#ifndef XIR_INVOCATION_DRIVER_LOOKUP_LIFETIME_CASES_H
#define XIR_INVOCATION_DRIVER_LOOKUP_LIFETIME_CASES_H
typedef struct DriverLookupTrial {
    InvocationClassCellFixture fixture;
    XrXirArtifact *checked;
    XrXirEffects *published;
    XrXirEffects effects;
    EffectGraph graph;
    EffectInvocationDeclaredBounds *bounds;
    EffectInvocationWorkspace *owner;
    EffectFinalFunctions *staged;
    EffectInvocationCertificate *certificate;
} DriverLookupTrial;
typedef struct DriverLookupOracle {
    EffectInvocationNode nodes[32];EffectInvocationEdge edges[64];
    unsigned char payloads[32][2048];size_t bytes[32];
    uint32_t count,edge_count,roots[6],spaces[6],node_slots[64],edge_slots[64];
    size_t node_capacity,edge_capacity;
} DriverLookupOracle;
static void *driver_lookup_node_raw,*driver_lookup_edge_raw;
static unsigned driver_lookup_dead;
static size_t driver_lookup_receiving;
static XrCompileResources *driver_lookup_sampling;
static uint64_t driver_lookup_expected_live,driver_lookup_work_entry,driver_lookup_work_cost;

static size_t driver_lookup_block_bytes(const void *payload) {
    const void *raw=(const CompileAllocation *)payload-1;
    for (size_t at=0;at<rp_live;++at) if (rp_blocks[at].pointer==raw) return rp_blocks[at].bytes;
    CHECK(false);return 0;
}
static void driver_lookup_release(void *raw) {
    if (!(driver_lookup_dead&1) && raw==driver_lookup_node_raw) driver_lookup_dead|=1;
    if (!(driver_lookup_dead&2) && raw==driver_lookup_edge_raw) driver_lookup_dead|=2;
}
static void driver_lookup_receive(void *raw) {
    (void)raw;CHECK(driver_lookup_dead==3 && driver_lookup_sampling);
    if (!driver_lookup_receiving) CHECK(driver_lookup_sampling->stats.live_bytes==driver_lookup_expected_live);
    ++driver_lookup_receiving;
}
static void driver_lookup_capture(EffectInvocationWorkspace *owner,DriverLookupOracle *oracle) {
    CHECK(owner->final.count<=32 && owner->final.edge_count<=64 && effect_body_count(owner->final.material)==6);
    oracle->count=owner->final.count;oracle->edge_count=owner->final.edge_count;
    memcpy(oracle->nodes,owner->final.nodes,(size_t)oracle->count*sizeof(*oracle->nodes));
    memcpy(oracle->edges,owner->final.edges,(size_t)oracle->edge_count*sizeof(*oracle->edges));
    memcpy(oracle->roots,owner->final.roots,sizeof(oracle->roots));memcpy(oracle->spaces,owner->final.root_spaces,sizeof(oracle->spaces));
    oracle->node_capacity=owner->node_index_capacity;oracle->edge_capacity=owner->edge_index_capacity;
    CHECK(oracle->node_capacity<=64 && oracle->edge_capacity<=64);
    memcpy(oracle->node_slots,owner->node_index,oracle->node_capacity*sizeof(uint32_t));
    memcpy(oracle->edge_slots,owner->edge_index,oracle->edge_capacity*sizeof(uint32_t));
    for (uint32_t n=0;n<oracle->count;++n) {
        oracle->bytes[n]=driver_lookup_block_bytes(owner->final.nodes[n].causes)-sizeof(CompileAllocation);
        CHECK(oracle->bytes[n]<=sizeof(oracle->payloads[n]));
        memcpy(oracle->payloads[n],owner->final.nodes[n].causes,oracle->bytes[n]);
    }
}
static void driver_lookup_match(EffectInvocationWorkspace *owner,const DriverLookupOracle *oracle) {
    CHECK(owner->final.count==oracle->count && owner->final.edge_count==oracle->edge_count && !owner->pending);
    CHECK(!memcmp(owner->final.nodes,oracle->nodes,(size_t)oracle->count*sizeof(*oracle->nodes)) &&
        !memcmp(owner->final.edges,oracle->edges,(size_t)oracle->edge_count*sizeof(*oracle->edges)) &&
        !memcmp(owner->final.roots,oracle->roots,sizeof(oracle->roots)) && !memcmp(owner->final.root_spaces,oracle->spaces,sizeof(oracle->spaces)));
    for (uint32_t n=0;n<oracle->count;++n) CHECK(!memcmp(owner->final.nodes[n].causes,oracle->payloads[n],oracle->bytes[n]));
    if (owner->node_index || owner->edge_index) {
        CHECK(owner->node_index && owner->edge_index && owner->node_index_capacity==oracle->node_capacity && owner->edge_index_capacity==oracle->edge_capacity);
        CHECK(!memcmp(owner->node_index,oracle->node_slots,oracle->node_capacity*sizeof(uint32_t)) &&
            !memcmp(owner->edge_index,oracle->edge_slots,oracle->edge_capacity*sizeof(uint32_t)));
    } else CHECK(!owner->node_index_capacity && !owner->edge_index_capacity);
}
static XrXirStatus driver_lookup_prepare(const XrXirCompileContext *context,DriverLookupTrial *trial) {
    memset(trial,0,sizeof(*trial));invocation_class_cell_fixture(&trial->fixture,0);
    XrXirStatus status=xir_fixture_check(context,&trial->fixture.source.module,&trial->checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(trial->checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(trial->checked,&trial->published);
    const XrXirModule *source=trial->checked?xr_xir_compile_artifact_module(trial->checked):NULL;
    if (status==XR_XIR_OK) status=invocation_core_storage(context,source,&trial->effects);
    if (status==XR_XIR_OK) status=effect_graph_build(source,&trial->effects,&trial->graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(source,&trial->effects,&trial->graph,context);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(context,&trial->bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<source->function_count;++f)
        status=effect_invocation_bounds_capture(context,trial->bounds,source->types,&source->functions[f],f,NULL);
    if (status==XR_XIR_OK) status=effect_invocations_receive(context,source,&trial->effects,&trial->graph,trial->bounds,&trial->owner);
    if (status==XR_XIR_OK) status=effect_invocations_solve(trial->owner);
    if (status==XR_XIR_OK) status=effect_invocation_stage(trial->owner,&trial->staged);
    return status;
}
static void driver_lookup_drop_producers(DriverLookupTrial *trial) {
    effect_invocation_bounds_free(trial->bounds);trial->bounds=NULL;effect_graph_free(&trial->graph);trial->graph=(EffectGraph){0};
    invocation_core_storage_free(&trial->effects);xr_xir_compile_artifact_free(trial->checked);trial->checked=NULL;
    memset(&trial->fixture,0xCE,sizeof(trial->fixture));
}
static void driver_lookup_free(DriverLookupTrial *trial) {
    effect_invocation_result_free(trial->staged);effect_invocation_free(trial->owner);driver_lookup_drop_producers(trial);
    effect_invocation_certificate_free(trial->certificate);xr_xir_compile_effects_free(trial->published);
}

static void driver_lookup_guard_controls(EffectInvocationWorkspace *owner) {
    RootParameterMark held=rp_mark();XrCompileResourceStats before=rp_stats(owner->work);uint32_t pending=owner->pending;
    owner->pending=1;CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BAD_STRUCTURE);owner->pending=pending;
    EffectBodyMaterial *material=owner->final.material;owner->final.material=NULL;
    CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BAD_STRUCTURE);owner->final.material=material;
    size_t nodes=owner->node_index_capacity,edges=owner->edge_index_capacity;
    owner->node_index_capacity=0;CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BAD_STRUCTURE);owner->node_index_capacity=nodes;
    owner->edge_index_capacity=edges+1;CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BAD_STRUCTURE);owner->edge_index_capacity=edges;
    uint32_t count=owner->final.edge_count;owner->final.edge_count=0;
    CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BAD_STRUCTURE);owner->final.edge_count=count;
    count=owner->final.count;owner->final.count=owner->final.capacity+1;
    CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BAD_STRUCTURE);owner->final.count=count;
    const XrXirCompileContext *work=owner->work;XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_before=rp_stats(&foreign);
    owner->work=&foreign;CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BAD_STRUCTURE);owner->work=work;
    CHECK(rp_stats(&foreign).work==foreign_before.work && rp_stats(&foreign).allocated_bytes==foreign_before.allocated_bytes);
    rp_owner_free(&foreign,foreign_before.live_bytes);rp_balanced(held);
    CHECK(!driver_lookup_dead && rp_stats(work).live_bytes==before.live_bytes &&
        rp_stats(work).allocated_bytes==before.allocated_bytes && rp_stats(work).work==before.work);
}

/* The unretired direct seal and the retired private-driver state are tested
 * separately. Both keep their old inputs on failure and retry the same owner. */
static XrXirStatus driver_lookup_run(const XrXirCompileContext *context,unsigned mode) {
    DriverLookupTrial trial;XrXirStatus status=driver_lookup_prepare(context,&trial);
    if (status!=XR_XIR_OK) { driver_lookup_free(&trial);return status; }
    EffectInvocationWorkspace *owner=trial.owner;DriverLookupOracle oracle;driver_lookup_capture(owner,&oracle);
    CHECK(owner->node_index && owner->edge_index && !rp_release_observer && !rp_allocation_observer);
    uint32_t *nodes=owner->node_index,*edges=owner->edge_index;size_t node_capacity=owner->node_index_capacity,edge_capacity=owner->edge_index_capacity;
    size_t bytes=driver_lookup_block_bytes(nodes)+driver_lookup_block_bytes(edges);
    CHECK(bytes==(node_capacity+edge_capacity)*sizeof(uint32_t)+2*sizeof(CompileAllocation));
    driver_lookup_node_raw=(CompileAllocation *)nodes-1;driver_lookup_edge_raw=(CompileAllocation *)edges-1;
    driver_lookup_dead=0;driver_lookup_receiving=0;driver_lookup_sampling=context->resources;
    XrCompileResourceStats before=rp_stats(context);driver_lookup_expected_live=before.live_bytes-bytes;
    RootParameterMark held=rp_mark();size_t attempts=rp_attempts;
    if (mode!=1) {
        rp_release_observer=driver_lookup_release;
        if (mode==4) { driver_lookup_guard_controls(owner);attempts=rp_attempts;driver_lookup_match(owner,&oracle); }
        driver_lookup_work_entry=rp_stats(context).work;status=effect_invocation_driver_lookups_retire(owner);
        if (mode==3) {
            CHECK(status==XR_XIR_BUDGET && !driver_lookup_dead && owner->node_index==nodes && owner->edge_index==edges &&
                owner->node_index_capacity==node_capacity && owner->edge_index_capacity==edge_capacity && rp_attempts==attempts);
            driver_lookup_match(owner,&oracle);rp_balanced(held);
            CHECK(effect_invocation_driver_lookups_retire(owner)==XR_XIR_BUDGET && !driver_lookup_dead && rp_attempts==attempts);
            rp_release_observer=NULL;driver_lookup_sampling=NULL;driver_lookup_free(&trial);return status;
        }
        if (status!=XR_XIR_OK) { rp_release_observer=NULL;driver_lookup_sampling=NULL;driver_lookup_free(&trial);return status; }
        driver_lookup_work_cost=rp_stats(context).work-driver_lookup_work_entry;
        CHECK(driver_lookup_dead==3 && !owner->node_index && !owner->edge_index && !owner->node_index_capacity && !owner->edge_index_capacity &&
            rp_attempts==attempts && rp_stats(context).allocated_bytes==before.allocated_bytes && rp_stats(context).live_bytes==driver_lookup_expected_live);
        driver_lookup_match(owner,&oracle);rp_allocation_observer=driver_lookup_receive;
    }
    EffectInvocationSealRequest request={&trial.owner,&trial.staged,NULL};
    if (mode==1 || mode==2) {
        RootParameterMark retained=rp_mark();uint64_t live=rp_stats(context).live_bytes;EffectFinalFunctions *staged=trial.staged;
        rp_fail_at=rp_attempts;rp_injected=false;size_t first=rp_attempts;
        status=effect_invocation_seal(context,&request,&trial.certificate,NULL);rp_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==first+1 && trial.owner==owner && trial.staged==staged && !trial.certificate);
        driver_lookup_match(owner,&oracle);rp_balanced(retained);CHECK(rp_stats(context).live_bytes==live);
        if (mode==1) CHECK(owner->node_index==nodes && owner->edge_index==edges && owner->node_index_capacity==node_capacity && owner->edge_index_capacity==edge_capacity);
        else CHECK(!owner->node_index && !owner->edge_index && driver_lookup_dead==3);
    }
    status=effect_invocation_seal(context,&request,&trial.certificate,NULL);
    rp_allocation_observer=NULL;rp_release_observer=NULL;driver_lookup_sampling=NULL;
    if (status==XR_XIR_OK) {
        CHECK(!trial.owner && !trial.staged && trial.certificate && (mode==1 || (driver_lookup_dead==3 && driver_lookup_receiving)));
        driver_lookup_drop_producers(&trial);
        EffectInvocationWorkspace replay={0};
        status=effect_invocation_read_open(context,trial.published,trial.certificate->declared,trial.certificate->final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,trial.certificate->body);
        if (status==XR_XIR_OK) status=effect_invocation_class_verify_read(&replay,&body);
        effect_invocation_read_close(&replay);
        if (status==XR_XIR_OK) status=effect_invocation_certificate_result_match(context,trial.certificate,&trial.certificate->final->summary);
    }
    driver_lookup_free(&trial);return status;
}

static void invocation_driver_lookup_lifetime_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(driver_lookup_run(&context,0)==XR_XIR_OK);XrCompileResourceStats measured=rp_stats(&context);
    uint64_t cut=driver_lookup_work_entry+driver_lookup_work_cost-1;rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned mode=1;mode<=4;++mode) {
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();if (mode==3) limits.work=cut;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(driver_lookup_run(&context,mode)==(mode==3?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(driver_lookup_run(&context,0)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Driver lookup lifetime: genuine Cell/Class complete solve/stage, both actual indexes die before first seal receiving with exact live/header accounting and no cumulative refund; complete node/edge/root and every cause/input/output payload unchanged, direct unretired seal and retired-owner actual first OOM/same finite-owner retries, retirement work-minusone/exhausted retry and all7 guard refusals, independent Class/result after producer death; whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_DRIVER_LOOKUP_LIFETIME_CASES_H
