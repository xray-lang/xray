/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_head_projection_cases.h - Complete current Cell head fields and gates
 */
#ifndef XIR_CELL_HEAD_PROJECTION_CASES_H
#define XIR_CELL_HEAD_PROJECTION_CASES_H
static uint32_t cell_head_reads;
static bool cell_head_refuse;
static XrXirStatus cell_head_delegate(const EffectBodyRequest *request,uint32_t f,
    EffectBodyFunctionHead *output,EffectBodyFunctionDimensions *dimensions) {
    ++cell_head_reads;if(cell_head_refuse)return XR_XIR_IO;
    return effect_body_owned_head(request,f,output,dimensions);
}
static bool cell_head_same(const XrXirCellProofBodyHead *a,const XrXirCellProofBodyHead *b) {
    return a->parameters==b->parameters && a->parameter_count==b->parameter_count && a->result==b->result &&
        a->instruction_count==b->instruction_count && a->operands==b->operands && a->operand_count==b->operand_count;
}
static void cell_head_faults(const XrXirCompileContext *context,EffectCellBodyAdapter *adapter) {
    EffectBodyMaterial *material=(EffectBodyMaterial *)adapter->request.owner;EffectBodyFunction *head=&material->functions[5];
    EffectBodyFunction saved=*head;CHECK(saved.parameter_count==1 && saved.instruction_count==2);
    const XrXirCellProofBodyHead sentinel={.parameters=saved.parameters,.parameter_count=19,.result=XR_XIR_BOOL,
        .instruction_count=27,.operands=saved.operands,.operand_count=31};
    RootParameterMark held=rp_mark();size_t attempts=rp_attempts;
    for(unsigned field=0;field<7;++field){*head=saved;
        if(!field){head->name=NULL;head->name_length=1;}
        else if(field==1)head->parameters=NULL;
        else if(field==2){head->blocks=NULL;head->block_count=1;}
        else if(field==3){head->operands=NULL;head->operand_count=1;}
        else if(field==4)head->runs=NULL;else if(field==5)head->run_count=0;else head->run_count=head->run_capacity+1;
        XrXirCellProofBodyHead output=sentinel;
        CHECK(effect_cell_body_head(context,adapter,5,&output)==XR_XIR_BAD_STRUCTURE && cell_head_same(&output,&sentinel));
        *head=saved;
    }
    EffectBodyRequest original=adapter->request;XrXirGeneric foreign_generics={0};
    XrXirDefaultTable foreign_defaults={0};XrXirDeclarations foreign_declarations={0};
    for(unsigned field=0;field<5;++field){
        if(!field)adapter->request.domain.types=&effect_empty_types;
        else if(field==1)adapter->request.domain.declarations=&foreign_declarations;
        else if(field==2)adapter->request.domain.generics=&foreign_generics;
        else if(field==3)adapter->request.domain.defaults=&foreign_defaults;
        else ++adapter->request.domain.function_count;
        XrXirCellProofBodyHead output=sentinel;
        CHECK(effect_cell_body_head(context,adapter,5,&output)==XR_XIR_BAD_STRUCTURE && cell_head_same(&output,&sentinel));
        adapter->request=original;
    }
    XrXirCellProofBodyHead output=sentinel;
    CHECK(effect_cell_body_head(context,adapter,6,&output)==XR_XIR_BAD_STRUCTURE && cell_head_same(&output,&sentinel));
    CHECK(rp_attempts==attempts);rp_balanced(held);
    XrXirCompileContext foreign=rp_owner(rp_caps());XrCompileResourceStats foreign_before=rp_stats(&foreign);attempts=rp_attempts;
    XrCompileResources *resources=material->resources;material->resources=foreign.resources;output=sentinel;
    CHECK(effect_cell_body_head(context,adapter,5,&output)==XR_XIR_BAD_STRUCTURE && cell_head_same(&output,&sentinel));material->resources=resources;
    adapter->request.read.owner=&foreign;output=sentinel;
    CHECK(effect_cell_body_head(context,adapter,5,&output)==XR_XIR_BAD_STRUCTURE && cell_head_same(&output,&sentinel));adapter->request=original;
    output=sentinel;CHECK(effect_cell_body_head(&foreign,adapter,5,&output)==XR_XIR_BAD_STRUCTURE && cell_head_same(&output,&sentinel));
    CHECK(rp_attempts==attempts && rp_stats(&foreign).work==foreign_before.work &&
        rp_stats(&foreign).allocated_bytes==foreign_before.allocated_bytes && rp_stats(&foreign).live_bytes==foreign_before.live_bytes);
    rp_owner_free(&foreign,foreign_before.live_bytes);rp_balanced(held);
    uint64_t exact=11+6+17+sizeof(output),original_limit=context->resources->limits.work;
    for(uint64_t cut=0;cut<=exact;++cut){uint64_t entry=rp_stats(context).work;CHECK(exact<=original_limit-entry);
        context->resources->limits.work=entry+cut;output=sentinel;attempts=rp_attempts;
        XrXirStatus status=effect_cell_body_head(context,adapter,5,&output);
        CHECK(status==(cut==exact?XR_XIR_OK:XR_XIR_BUDGET) && rp_attempts==attempts);
        if(cut<exact){CHECK(cell_head_same(&output,&sentinel));
            CHECK(effect_cell_body_head(context,adapter,5,&output)==XR_XIR_BUDGET && cell_head_same(&output,&sentinel) && rp_attempts==attempts);}
        context->resources->limits.work=original_limit;rp_balanced(held);
    }
}
static XrXirStatus cell_head_run(const XrXirCompileContext *context,bool controls) {
    DriverLookupTrial trial;XrXirStatus status=driver_lookup_prepare(context,&trial);
    EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
    if(status==XR_XIR_OK)status=effect_cell_body_owned(context,trial.owner->final.material,&adapter,&read);
    for(uint32_t f=0;status==XR_XIR_OK && f<6;++f){
        XrXirCellProofBodyHead output={0},oracle={0};XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
        status=effect_cell_body_head(context,&adapter,f,&output);
        if(status!=XR_XIR_OK)break;
        CHECK(rp_stats(context).work-before.work==11+6+17+sizeof(output) && rp_attempts==attempts);
        EffectCellBodyAdapter delegated=adapter;delegated.request.head=cell_head_delegate;cell_head_reads=0;cell_head_refuse=false;
        before=rp_stats(context);status=effect_cell_body_head(context,&delegated,f,&oracle);
        if(status!=XR_XIR_OK)break;
        CHECK(cell_head_reads==1 && cell_head_same(&output,&oracle) && rp_attempts==attempts &&
            rp_stats(context).work-before.work==28+sizeof(EffectBodyFunctionHead)+sizeof(oracle));
        if(controls){const XrXirCellProofBodyHead sentinel={.parameter_count=19,.result=XR_XIR_BOOL,.instruction_count=27};oracle=sentinel;cell_head_refuse=true;
            CHECK(effect_cell_body_head(context,&delegated,f,&oracle)==XR_XIR_IO && cell_head_reads==2 && cell_head_same(&oracle,&sentinel) && rp_attempts==attempts);
            cell_head_refuse=false;
        }
    }
    if(status==XR_XIR_OK && controls)cell_head_faults(context,&adapter);
    if(status==XR_XIR_OK){xr_xir_compile_artifact_free(trial.checked);trial.checked=NULL;memset(&trial.fixture,0xCE,sizeof(trial.fixture));
        status=effect_cell_body_project(&adapter);
        if(status==XR_XIR_OK)status=xr_xir_compile_cell_provenance_body_matches(context,trial.owner->final.cells,&read,NULL);
    }
    effect_cell_body_projection_drop(&adapter);driver_lookup_free(&trial);return status;
}
static void cell_head_projection_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(cell_head_run(&context,false)==XR_XIR_OK);XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;
    CHECK(cell_head_run(&context,true)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits limits=rp_caps();
        if(!axis)limits.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(cell_head_run(&context,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    fprintf(stderr,"Cell head fields: genuine Cell/Class Checked and complete solve/stage, all6 actual heads match original complete delegated oracle and every IO refusal, all7 full presence and5 domain guards plus foreign ledger/output atomicity/no allocation, every work-unit cap through exact and same finite-owner refusal, fresh independent Cell after Checked/producer death; whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_CELL_HEAD_PROJECTION_CASES_H
