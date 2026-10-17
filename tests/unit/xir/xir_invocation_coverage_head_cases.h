/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_coverage_head_cases.h - Current complete head argument reads
 */
#ifndef XIR_INVOCATION_COVERAGE_HEAD_CASES_H
#define XIR_INVOCATION_COVERAGE_HEAD_CASES_H
static uint32_t coverage_head_target,coverage_head_full_reads;
static size_t coverage_head_refused=SIZE_MAX;
static XrCompileResources *coverage_head_sampling;
static uint64_t coverage_head_receiving_cut;
static size_t coverage_head_receiving_count;
/* Calloc admits its complete zeroing fee before the raw allocator runs.
 * Exhaust work after that admitted allocation, before argument population. */
static void coverage_head_observe(void *pointer) {
    CHECK(coverage_head_sampling && !coverage_head_receiving_count && rp_live);
    RootParameterAllocation allocation=rp_blocks[rp_live-1];
    CHECK(allocation.pointer==pointer && allocation.bytes>=sizeof(CompileAllocation));
    coverage_head_receiving_cut=coverage_head_sampling->stats.work+allocation.bytes-sizeof(CompileAllocation);
    ++coverage_head_receiving_count;
}
static XrXirStatus coverage_head_delegated(const EffectBodyRequest *request,uint32_t function,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions) {
    if (output && function==coverage_head_target && coverage_head_full_reads++==coverage_head_refused)
        return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_head(request,function,output,dimensions);
}

/* The original delegated adapter must still perform both actual full-head
 * callbacks. Each refusal remains observable before argument allocation. */
static void coverage_head_faults(EffectInvocationFlow *flow,uint32_t edge) {
    const XrXirCompileContext *work=flow->owner->work;EffectBodyRequest original=flow->body;
    EffectInvocationNode *target=&flow->owner->final.nodes[flow->owner->final.edges[edge].target];
    CHECK(target->body==1 && target->parameter_count==2);
    coverage_head_target=target->body;flow->body.head=coverage_head_delegated;
    bool matched=false;coverage_head_full_reads=0;coverage_head_refused=SIZE_MAX;
    RootParameterMark held=rp_mark();uint64_t live=rp_stats(work).live_bytes;
    CHECK(effect_invocation_coverage_input(flow,edge,&matched)==XR_XIR_OK && matched && coverage_head_full_reads==2);
    rp_balanced(held);CHECK(rp_stats(work).live_bytes==live);
    for (size_t at=0;at<2;++at) {
        coverage_head_full_reads=0;coverage_head_refused=at;matched=true;size_t attempts=rp_attempts;
        CHECK(effect_invocation_coverage_input(flow,edge,&matched)==XR_XIR_BAD_STRUCTURE && !matched &&
            coverage_head_full_reads==at+1 && rp_attempts==attempts);rp_balanced(held);
    }
    coverage_head_refused=SIZE_MAX;flow->body=original;
    EffectBodyFunction *head=&flow->owner->final.material->functions[target->body];EffectBodyFunction saved=*head;
    CHECK(saved.parameter_count==2 && saved.instruction_count);
    for (unsigned field=0;field<7;++field) {
        *head=saved;
        if (!field) { head->name=NULL;head->name_length=1; }
        else if (field==1) head->parameters=NULL;
        else if (field==2) { head->blocks=NULL;head->block_count=1; }
        else if (field==3) { head->operands=NULL;head->operand_count=1; }
        else if (field==4) head->runs=NULL;
        else if (field==5) head->run_count=0;
        else head->run_count=head->run_capacity+1;
        matched=true;size_t attempts=rp_attempts;
        CHECK(effect_invocation_coverage_input(flow,edge,&matched)==XR_XIR_BAD_STRUCTURE && !matched && rp_attempts==attempts);
        *head=saved;rp_balanced(held);
    }
    uint64_t words=(uint64_t)target->parameter_count*flow->basis.row_words;CHECK(words && words<=16);
    for (size_t word=0;word<(size_t)words;++word) {
        target->input[word]^=UINT64_C(1)<<40;matched=true;
        CHECK(effect_invocation_coverage_input(flow,edge,&matched)==XR_XIR_OK && !matched);
        target->input[word]^=UINT64_C(1)<<40;rp_balanced(held);
    }
    matched=false;CHECK(effect_invocation_coverage_input(flow,edge,&matched)==XR_XIR_OK && matched);
    CHECK(rp_stats(work).live_bytes==live);rp_balanced(held);
}

/* The genuine checker, complete equation solve and original origin replay
 * supply this current CALL and input. No numeric fixture grants admission. */
static XrXirStatus coverage_head_run(const XrXirCompileContext *context,unsigned mode,size_t site) {
    GenerationRangeTrial trial;generation_range_fixture(&trial,0);
    EffectInvocationWorkspace *owner=NULL;EffectInvocationFlow flow={0};
    XrXirStatus status=generation_range_prepare(context,&trial);
    if (status!=XR_XIR_OK) goto done;
    const XrXirModule *source=xr_xir_compile_artifact_module(trial.checked);
    EffectBodyMaterial *material=trial.effects.generation;
    status=effect_graph_build_material(source,&trial.effects,&trial.graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_invocations_receive(context,source,&trial.effects,&trial.graph,material->declared,&owner);
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    if (status!=XR_XIR_OK) goto done;
    flow=(EffectInvocationFlow){.owner=owner,.node=owner->final.roots[0],.instruction=UINT32_MAX};
    status=effect_invocation_origins(&flow);
    uint32_t selected=UINT32_MAX;
    for (uint32_t e=0;status==XR_XIR_OK && e<owner->final.edge_count;++e) {
        const EffectInvocationEdge *edge=&owner->final.edges[e];
        if (edge->caller!=flow.node || owner->final.nodes[edge->target].body!=1 || edge->latent || edge->producer!=UINT32_MAX) continue;
        bool matched=false;status=effect_invocation_coverage_input(&flow,e,&matched);
        if (status==XR_XIR_OK && matched) { selected=e;break; }
    }
    if (status!=XR_XIR_OK) goto done;
    CHECK(selected!=UINT32_MAX && !owner->pending);
    if (mode==4) coverage_head_faults(&flow,selected);
    EffectInvocationNode *target=&owner->final.nodes[owner->final.edges[selected].target],saved;
    memcpy(&saved,target,sizeof(saved));uint64_t words=(uint64_t)target->parameter_count*flow.basis.row_words,inputs[16];
    CHECK(words && words<=16);memcpy(inputs,target->input,(size_t)words*sizeof(*inputs));
    RootParameterMark held=rp_mark();uint64_t live=rp_stats(context).live_bytes;size_t attempts=rp_attempts;
    uint32_t nodes=owner->final.count,edges=owner->final.edge_count;
    if (mode==1) {
        CHECK(!rp_allocation_observer);coverage_head_sampling=context->resources;coverage_head_receiving_count=0;
        rp_allocation_observer=coverage_head_observe;
    }
    if (mode==2) { rp_fail_at=rp_attempts+site;rp_injected=false; }
    bool matched=true;status=effect_invocation_coverage_input(&flow,selected,&matched);
    if (mode==1) { rp_allocation_observer=NULL;coverage_head_sampling=NULL; }
    if (mode==2) rp_fail_at=SIZE_MAX;
    if (mode==2 || mode==3) {
        if (status!=(mode==2?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET) || matched || rp_attempts!=attempts+site+1)
            fprintf(stderr,"Coverage resource control mode%u site%llu status%u matched%u attempts%llu expected%llu work%llu\n",
                mode,(unsigned long long)site,(unsigned)status,matched,(unsigned long long)(rp_attempts-attempts),
                (unsigned long long)(site+1),(unsigned long long)rp_stats(context).work);
        CHECK(status==(mode==2?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET) && !matched && rp_attempts==attempts+site+1);
        CHECK(mode!=2 || rp_injected);
        CHECK(owner->final.count==nodes && owner->final.edge_count==edges && !owner->pending &&
            !memcmp(target,&saved,sizeof(saved)) && !memcmp(target->input,inputs,(size_t)words*sizeof(*inputs)) &&
            rp_stats(context).live_bytes==live);rp_balanced(held);
        if (mode==2) { status=effect_invocation_coverage_input(&flow,selected,&matched);CHECK(status!=XR_XIR_OK || matched); }
        else CHECK(effect_invocation_coverage_input(&flow,selected,&matched)==XR_XIR_BUDGET && !matched);
    }
    if (status==XR_XIR_OK) CHECK(matched && !memcmp(target,&saved,sizeof(saved)) &&
        !memcmp(target->input,inputs,(size_t)words*sizeof(*inputs)) && rp_stats(context).live_bytes==live);
    rp_balanced(held);
done:
    xr_compile_resources_free(flow.memory);effect_invocation_free(owner);
    effect_graph_free(&trial.graph);invocation_core_storage_free(&trial.effects);xr_xir_compile_artifact_free(trial.checked);
    return status;
}

static void invocation_coverage_head_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;CHECK(coverage_head_run(&context,1,0)==XR_XIR_OK);
    XrCompileResourceStats measured=rp_stats(&context);size_t sites=coverage_head_receiving_count;CHECK(sites==1);
    uint64_t cut=coverage_head_receiving_cut;rp_owner_free(&context,baseline);rp_balanced(physical);
    for (size_t at=0;at<sites;++at) {
        physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        CHECK(coverage_head_run(&context,2,at)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();limits.work=cut;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(coverage_head_run(&context,3,at)==XR_XIR_BUDGET);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(coverage_head_run(&context,4,0)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(coverage_head_run(&context,0,0)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Current coverage head: genuine Checked/complete solve/origin/CALL, delegated two full-head callbacks and both refusals, all7 head shape and every target input-word mismatch controls; all%zu actual argument OOM/admitted-allocation work cuts and same finite-owner retries preserve complete old node/input, whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_COVERAGE_HEAD_CASES_H
