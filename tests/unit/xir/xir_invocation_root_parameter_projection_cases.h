/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_root_parameter_projection_cases.h - Current complete root heads
 */
#ifndef XIR_INVOCATION_ROOT_PARAMETER_PROJECTION_CASES_H
#define XIR_INVOCATION_ROOT_PARAMETER_PROJECTION_CASES_H
static uint32_t root_parameter_head_reads;
static bool root_parameter_head_refused;
static XrXirStatus root_parameter_head(const EffectBodyRequest *request,uint32_t function,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions) {
    if (output && function==5) { ++root_parameter_head_reads;if (root_parameter_head_refused) return XR_XIR_BAD_STRUCTURE; }
    return effect_body_owned_head(request,function,output,dimensions);
}

static void root_parameter_faults(EffectInvocationFlow *flow,const uint64_t *row) {
    EffectInvocationWorkspace *owner=flow->owner;const XrXirCompileContext *work=owner->work;
    CHECK(owner->final.nodes[flow->node].root==5 && flow->basis.parameters==1);
    EffectBodyRequest original=flow->body;RootParameterMark held=rp_mark();uint64_t live=rp_stats(work).live_bytes;
    flow->body.head=root_parameter_head;root_parameter_head_reads=0;root_parameter_head_refused=false;
    bool changed=false;size_t attempts=rp_attempts;
    CHECK(effect_invocation_cell_consume(flow,row,&changed)==XR_XIR_OK && !changed && root_parameter_head_reads==1 && rp_attempts==attempts);
    root_parameter_head_refused=true;changed=false;
    CHECK(effect_invocation_cell_consume(flow,row,&changed)==XR_XIR_BAD_STRUCTURE && !changed && root_parameter_head_reads==2 && rp_attempts==attempts);
    root_parameter_head_refused=false;flow->body=original;rp_balanced(held);
    EffectBodyFunction *head=&owner->final.material->functions[5];EffectBodyFunction saved=*head;
    CHECK(saved.parameter_count==1 && saved.instruction_count==2 && saved.parameters);
    for (unsigned field=0;field<8;++field) {
        *head=saved;
        if (!field) { head->name=NULL;head->name_length=1; }
        else if (field==1) head->parameters=NULL;
        else if (field==2) { head->blocks=NULL;head->block_count=1; }
        else if (field==3) { head->operands=NULL;head->operand_count=1; }
        else if (field==4) head->runs=NULL;
        else if (field==5) head->run_count=0;
        else if (field==6) head->run_count=head->run_capacity+1;
        else ++head->parameter_count;
        changed=false;
        CHECK(effect_invocation_cell_consume(flow,row,&changed)==XR_XIR_BAD_STRUCTURE && !changed && rp_attempts==attempts);
        *head=saved;rp_balanced(held);
    }
    XrXirType *parameter=(XrXirType *)head->parameters;XrXirType saved_type=*parameter;*parameter=XR_XIR_I64;changed=false;
    CHECK(effect_invocation_cell_consume(flow,row,&changed)==XR_XIR_BAD_STRUCTURE && !changed && rp_attempts==attempts);
    *parameter=saved_type;rp_balanced(held);
    EffectBodyMaterial *material=owner->final.material;owner->final.material=NULL;changed=false;
    CHECK(effect_invocation_cell_consume(flow,row,&changed)==XR_XIR_BAD_STRUCTURE && !changed && rp_attempts==attempts);
    owner->final.material=material;
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats before=rp_stats(&foreign);attempts=rp_attempts;
    flow->body.read.owner=&foreign;changed=false;
    CHECK(effect_invocation_cell_consume(flow,row,&changed)==XR_XIR_BAD_STRUCTURE && !changed && rp_attempts==attempts);
    CHECK(rp_stats(&foreign).allocated_bytes==before.allocated_bytes && rp_stats(&foreign).live_bytes==before.live_bytes && rp_stats(&foreign).work==before.work);
    flow->body=original;rp_owner_free(&foreign,before.live_bytes);rp_balanced(held);
    changed=false;CHECK(effect_invocation_cell_consume(flow,row,&changed)==XR_XIR_OK && !changed && rp_stats(work).live_bytes==live);
    rp_balanced(held);
}

/* A real root Cell formal and its actual access are solved before caller
 * storage dies. All later reads use the retained immutable body and inputs. */
static XrXirStatus root_parameter_run(const XrXirCompileContext *context,bool controls) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,0);
    XrXirArtifact *checked=NULL;XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationWorkspace *owner=NULL;
    EffectInvocationDeclaredBounds *bounds=NULL;EffectInvocationFlow flow={0};uint64_t *row=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    const XrXirModule *source=checked?xr_xir_compile_artifact_module(checked):NULL;
    if (status==XR_XIR_OK) status=invocation_core_storage(context,source,&effects);
    if (status==XR_XIR_OK) status=effect_graph_build(source,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(source,&effects,&graph,context);
    if (status==XR_XIR_OK) status=effect_invocation_bounds_new(context,&bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<source->function_count;++f)
        status=effect_invocation_bounds_capture(context,bounds,source->types,&source->functions[f],f,NULL);
    if (status==XR_XIR_OK) status=effect_invocations_receive(context,source,&effects,&graph,bounds,&owner);
    if (status==XR_XIR_OK) status=effect_invocations_solve(owner);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if (status==XR_XIR_OK) {
        CHECK(!owner->pending && owner->final.roots[5]<owner->final.count);
        flow=(EffectInvocationFlow){.owner=owner,.node=owner->final.roots[5],.instruction=UINT32_MAX,.precision=true};
        status=effect_invocation_origins(&flow);
    }
    if (status==XR_XIR_OK) {
        CHECK(flow.basis.parameters==1 && flow.function->parameter_count==1 && flow.function->instruction_count==2);
        row=xir_compile_calloc(context,flow.basis.cell_words,sizeof(*row),&status);
    }
    if (status==XR_XIR_OK) status=effect_invocation_cell(&flow,0,row,NULL);
    if (status==XR_XIR_OK) {
        CHECK(row && (row[0]&1));flow.instruction=0;bool changed=false;
        EffectInvocationNode *node=&owner->final.nodes[flow.node],saved;memcpy(&saved,node,sizeof(saved));
        CHECK(node->cause_count==5);EffectInvocationCause causes[10];memcpy(causes,node->causes,sizeof(causes));
        status=effect_invocation_cell_consume(&flow,row,&changed);
        if (status==XR_XIR_OK) {
            CHECK(!changed && !memcmp(node,&saved,sizeof(saved)) && !memcmp(node->causes,causes,sizeof(causes)));
            if (controls) root_parameter_faults(&flow,row);
            CHECK(!memcmp(node,&saved,sizeof(saved)) && !memcmp(node->causes,causes,sizeof(causes)));
        }
    }
    xr_compile_resources_free(row);xr_compile_resources_free(flow.memory);effect_invocation_free(owner);
    effect_invocation_bounds_free(bounds);effect_graph_free(&graph);invocation_core_storage_free(&effects);return status;
}

static void invocation_root_parameter_projection_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(root_parameter_run(&context,false)==XR_XIR_OK);XrCompileResourceStats measured=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(root_parameter_run(&context,true)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    for (unsigned axis=0;axis<3;++axis) for (unsigned minus=0;minus<2;++minus) {
        physical=rp_mark();XrCompileResourceLimits limits=rp_caps();
        if (!axis) limits.allocated_bytes=measured.allocated_bytes-minus;
        else if (axis==1) limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(root_parameter_run(&context,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Current root parameters: genuine Cell/Class Checked and complete solve, root Cell formal/access after producer death, complete node and independent local/global causes unchanged; delegated real full-head callback/refusal, all8 current head shape/dimension and actual parameter type/material/foreign ledger controls without allocation; whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_ROOT_PARAMETER_PROJECTION_CASES_H
