/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_record_owner_cases.h - Shared bytes with fresh semantic receiving
 */
#ifndef XIR_SNAPSHOT_RECORD_OWNER_CASES_H
#define XIR_SNAPSHOT_RECORD_OWNER_CASES_H
static XrCompileResources *record_owner_sampling;
static uint64_t record_owner_allocation_work[256];
static size_t record_owner_allocation_count;
static void record_owner_allocation_observe(void *pointer) {
    (void)pointer;CHECK(record_owner_sampling && record_owner_allocation_count<256);
    record_owner_allocation_work[record_owner_allocation_count++]=record_owner_sampling->stats.work;
}
static XrXirStatus record_owner_setup(const XrXirCompileContext *context,InvocationClassRowsFixture *fixture,
    XrXirArtifact **checked,EffectBodyMaterial **root) {
    invocation_class_rows_fixture(fixture,1);
    XrXirStatus status=xir_fixture_check(context,&fixture->module,checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(*checked,NULL);
    if(status==XR_XIR_OK)status=effect_body_material_create(context,xr_xir_compile_artifact_module(*checked),root);
    return status;
}
static XrXirStatus record_owner_current(const XrXirCompileContext *context,const EffectBodyMaterial *root,
    const EffectBodyMaterial *child) {
    CHECK(child && child->storage_kind==EFFECT_BODY_RECORDS_SHARED && child->records_owner==root &&
        !child->backbone && child->functions!=root->functions && child->domain.types!=root->domain.types &&
        child->cells!=root->cells && child->physical!=root->physical && child->declared!=root->declared &&
        child->control!=root->control && child->local_roots!=root->local_roots);
    for(uint32_t f=0;f<effect_body_count(root);++f)CHECK(child->functions[f].runs==root->functions[f].runs &&
        child->functions[f].run_count==root->functions[f].run_count &&
        child->functions[f].run_capacity==root->functions[f].run_capacity);
    XrXirStatus status=effect_body_physical_match(context,child->physical,child);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    if(status==XR_XIR_OK)status=effect_cell_body_owned(context,child,&adapter,&read);
    if(status==XR_XIR_OK)status=xr_xir_compile_cell_provenance_body_matches(context,child->cells,&read,NULL);
    return status;
}
static void record_owner_refusals(const XrXirCompileContext *context,EffectBodyMaterial *root,
    InvocationClassRowsFixture *fixture) {
    EffectBodyMaterial *selected=NULL;CHECK(effect_body_records_source(context,&fixture->module,root,&selected)==XR_XIR_OK && selected==root);
    for(uint32_t row=0;row<11;++row)for(uint32_t field=0;field<10;++field){XrXirInstruction held=fixture->entry[row];
        conditional_compact_change(&fixture->entry[row],field);
        if(field==0)fixture->entry[row].op=held.op==XR_XIR_CONST_BOOL?XR_XIR_CONST_INT:XR_XIR_CONST_BOOL;
        if(field==1)fixture->entry[row].type=held.type==XR_XIR_BOOL?XR_XIR_I64:XR_XIR_BOOL;
        CHECK(!conditional_compact_same(&held,&fixture->entry[row]));selected=NULL;
        CHECK(effect_body_records_source(context,&fixture->module,root,&selected)==XR_XIR_OK && !selected);
        fixture->entry[row]=held;
    }
    XrCompileResourceStats before=rp_stats(context);uint32_t kind=root->storage_kind;EffectBodyMaterial *parent=root->records_owner;
    selected=NULL;CHECK(effect_body_records_source(NULL,&fixture->module,root,&selected)==XR_XIR_BAD_STRUCTURE && !selected);
    XrXirModule missing=fixture->module;missing.functions=NULL;
    CHECK(effect_body_records_source(context,&missing,root,&selected)==XR_XIR_BAD_STRUCTURE && !selected);
    root->storage_kind=EFFECT_BODY_RECORDS_SHARED;root->records_owner=root;selected=NULL;
    CHECK(effect_body_records_source(context,&fixture->module,root,&selected)==XR_XIR_BAD_STRUCTURE && !selected);
    root->storage_kind=EFFECT_BODY_HEAD_OVERLAY;
    CHECK(effect_body_records_source(context,&fixture->module,root,&selected)==XR_XIR_BAD_STRUCTURE && !selected);
    root->storage_kind=kind;root->records_owner=parent;XrCompileResourceStats after=rp_stats(context);
    CHECK(!memcmp(&before,&after,sizeof(before)));
    EffectBodyRun *run=&root->functions[3].runs[0];uint32_t first=run->first;++run->first;
    CHECK(effect_body_records_source(context,&fixture->module,root,&selected)==XR_XIR_BAD_STRUCTURE && !selected);run->first=first;
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t baseline=rp_stats(&foreign).live_bytes;
    CHECK(effect_body_records_source(&foreign,&fixture->module,root,&selected)==XR_XIR_BAD_STRUCTURE && !selected);
    rp_owner_free(&foreign,baseline);
}
static XrXirStatus record_owner_operation(const XrXirCompileContext *context,size_t inject,bool observe) {
    InvocationClassRowsFixture fixture;XrXirArtifact *checked=NULL;EffectBodyMaterial *root=NULL,*child=NULL;
    XrXirStatus status=record_owner_setup(context,&fixture,&checked,&root);
    if(status==XR_XIR_OK){const XrXirModule *module=xr_xir_compile_artifact_module(checked);
        uint32_t references=root->references;RootParameterMark held=rp_mark();
        if(observe){CHECK(!rp_allocation_observer);record_owner_sampling=context->resources;record_owner_allocation_count=0;
            rp_allocation_observer=record_owner_allocation_observe;}
        if(inject!=SIZE_MAX){rp_fail_at=rp_attempts+inject;rp_injected=false;}
        status=effect_body_material_generation_input(context,module,NULL,NULL,root,&child);rp_fail_at=SIZE_MAX;
        if(observe){rp_allocation_observer=NULL;record_owner_sampling=NULL;}
        if(inject!=SIZE_MAX){CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && !child && root->references==references);
            rp_balanced(held);status=effect_body_material_generation_input(context,module,NULL,NULL,root,&child);CHECK(status==XR_XIR_OK);}
        if(status==XR_XIR_OK)status=record_owner_current(context,root,child);
        effect_body_material_free(child);child=NULL;CHECK(root->references==references);rp_balanced(held);
    }
    effect_body_material_free(child);effect_body_material_free(root);xr_xir_compile_artifact_free(checked);return status;
}
static void record_owner_lifetime(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    InvocationClassRowsFixture fixture;XrXirArtifact *checked=NULL;EffectBodyMaterial *root=NULL,*child=NULL,*last=NULL;
    CHECK(record_owner_setup(&context,&fixture,&checked,&root)==XR_XIR_OK);record_owner_refusals(&context,root,&fixture);
    XrXirInstruction expected[11];memcpy(expected,fixture.entry,sizeof(expected));const XrXirModule *module=xr_xir_compile_artifact_module(checked);
    CHECK(effect_body_material_generation_input(&context,module,NULL,NULL,root,&child)==XR_XIR_OK && root->references==2);
    CHECK(record_owner_current(&context,root,child)==XR_XIR_OK);
    CHECK(effect_body_material_generation_input(&context,module,NULL,NULL,child,&last)==XR_XIR_OK && root->references==3 && child->references==1);
    CHECK(record_owner_current(&context,root,last)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xce,sizeof(fixture));
    effect_body_material_free(root);effect_body_material_free(child);root=child=NULL;
    CHECK(last->records_owner->references==1 && record_owner_current(&context,last->records_owner,last)==XR_XIR_OK);
    EffectBodyRead read=effect_body_compile_read(&context);
    for(uint32_t i=0;i<11;++i){XrXirInstruction actual;CHECK(effect_body_instruction_get(&read,&last->functions[3],i,&actual)==XR_XIR_OK &&
        conditional_compact_same(&actual,&expected[i]));}
    effect_body_material_free(last);rp_owner_free(&context,baseline);rp_balanced(physical);
}
static void snapshot_record_owner_controls(void) {
    record_owner_lifetime();RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;CHECK(record_owner_operation(&context,SIZE_MAX,true)==XR_XIR_OK);
    size_t sites=record_owner_allocation_count;CHECK(sites);XrCompileResourceStats measured=rp_stats(&context);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for(size_t at=0;at<sites;++at){physical=rp_mark();context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
        CHECK(record_owner_operation(&context,at,false)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(size_t at=0;at<sites;++at){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();caps.work=record_owner_allocation_work[at];
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(record_owner_operation(&context,SIZE_MAX,false)==XR_XIR_BUDGET);
        RootParameterMark held=rp_mark();CHECK(record_owner_operation(&context,SIZE_MAX,false)==XR_XIR_BUDGET);rp_balanced(held);
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){physical=rp_mark();XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;
        CHECK(record_owner_operation(&context,SIZE_MAX,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));
        rp_owner_free(&context,baseline);rp_balanced(physical);}
    fprintf(stderr,"Record data lifetime: complete110 field refusals and malformed/self/kind/ledger checks, shared immutable rows with fresh physical/Cell/declared owners, flattened root after Checked/producer/external-parent deaths, independent full Cell and eleven-row oracles, all%zu actual child OOM/work cuts and same finite-ledger retries, six axes%llu/%llu/%llu exact/minusone PASS\n",
        sites,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_SNAPSHOT_RECORD_OWNER_CASES_H
