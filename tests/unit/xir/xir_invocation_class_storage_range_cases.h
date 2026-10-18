/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_class_storage_range_cases.h - Current physical Class receiving
 */
#ifndef XIR_INVOCATION_CLASS_STORAGE_RANGE_CASES_H
#define XIR_INVOCATION_CLASS_STORAGE_RANGE_CASES_H
/* Freeze the complete prior random reader, including failure cleanup. */
static XrXirStatus class_storage_random_oracle(EffectInvocationWorkspace *owner,
    uint32_t body,EffectInvocationClassFact **output,uint32_t *count) {
    if (!output || *output || !count || *count || body>=owner->body.domain.function_count ||
        !owner->final.physical || owner->final.physical->resources!=owner->work->resources ||
        owner->final.physical->count!=owner->body.domain.function_count)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyFunctionDimensions head={0};XrXirStatus status=effect_invocation_dimensions(owner,body,&head);
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunctionDimensions *function=&head;
    const EffectPhysicalFunction *physical=&owner->final.physical->functions[body];
    if (physical->values!=(uint64_t)function->parameter_count+function->instruction_count ||
        physical->classes>physical->values || physical->count>physical->capacity ||
        (physical->count && !physical->records)) return XR_XIR_BAD_STRUCTURE;
    if (!physical->classes) return XR_XIR_OK;
    EffectInvocationClassFact *facts=xir_compile_calloc(owner->work,
        physical->classes,sizeof(*facts),&status);
    if (!facts) return status;
    uint32_t at=0;
    for (uint32_t r=0;r<physical->count && status==XR_XIR_OK;++r) {
        if (!xir_compile_work(owner->work,1)) { status=XR_XIR_BUDGET;break; }
        EffectPhysicalRecord record=physical->records[r];
        if (!(record.mask&EFFECT_PHYSICAL_CLASS)) continue;
        XrXirType type;status=effect_body_request_type(&owner->body,body,record.value,&type);
        if (status!=XR_XIR_OK) break;
        if (at>=physical->classes || record.value>=physical->values ||
            !xr_xir_type_is_class(owner->body.domain.types,type) ||
            (at && facts[at-1].value>=record.value)) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (!xir_compile_work(owner->work,sizeof(*facts))) { status=XR_XIR_BUDGET;break; }
        facts[at++]=(EffectInvocationClassFact){.value=record.value};
    }
    if (status==XR_XIR_OK && at!=physical->classes) status=XR_XIR_BAD_STRUCTURE;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(facts);return status; }
    *output=facts;*count=at;return XR_XIR_OK;
}

typedef struct ClassStorageRangeFixture {
    InvocationClassRowsFixture nominal;
    EffectBodyMaterial material;
    EffectBodyFunction function;
    EffectPhysicalDomain physical;
    EffectPhysicalFunction membership;
    EffectPhysicalRecord records[26];
    XrXirType parameters[2];
    EffectInvocationWorkspace owner;
} ClassStorageRangeFixture;
static uint64_t class_storage_range_work,class_storage_range_cuts,class_storage_range_mutations;
static size_t class_storage_range_callbacks,class_storage_range_refused=SIZE_MAX;

/* This component fixture owns real rows and allocations but cannot publish
 * a semantic artifact. Genuine checked graphs are tested separately below. */
static XrXirStatus class_storage_range_fixture(const XrXirCompileContext *work,
    ClassStorageRangeFixture *f,uint32_t count,unsigned layout) {
    CHECK(count<=24 && layout<3);memset(f,0,sizeof(*f));invocation_class_rows_fixture(&f->nominal,0);
    f->parameters[0]=(XrXirType)256;f->parameters[1]=XR_XIR_I64;
    f->function=(EffectBodyFunction){.parameter_count=2,.parameters=f->parameters,.instruction_count=count};
    f->physical=(EffectPhysicalDomain){work->resources,1,&f->membership};
    f->material=(EffectBodyMaterial){.resources=work->resources,.references=1,.functions=&f->function,
        .domain={.function_count=1,.types=&f->material.terms.types},.physical=&f->physical};
    f->material.terms.types=f->nominal.types;CHECK(effect_body_material_shape(&f->material));
    f->membership=(EffectPhysicalFunction){.parameters=2,.instructions=count,.values=count+2,
        .count=count+2,.capacity=count+2,.records=f->records};
    XrXirInstruction rows[24];
    for(uint32_t v=0;v<count+2;++v){
        bool selected=v==0 || (v>=2 && (layout<2 || (v%3)!=1));
        f->records[v]=(EffectPhysicalRecord){v,selected?EFFECT_PHYSICAL_CLASS:0};
        if(selected)++f->membership.classes;
        if(v>=2){uint32_t i=v-2;rows[i]=(XrXirInstruction){.op=XR_XIR_COPY,
            .type=selected?(XrXirType)256:XR_XIR_I64,.args={layout?i*i:i,layout?i*i+3:0},
            .targets={layout?i*7:0,layout?i*i+9:0},.immediate=layout?(int64_t)i*i:0};}
    }
    XrXirFunction source={.instructions=count?rows:NULL,.instruction_count=count};
    XrXirStatus status=effect_body_records_copy(work,&source,&f->function);memset(rows,0xCE,sizeof(rows));
    f->owner=(EffectInvocationWorkspace){.work=work,.body=effect_body_owned_request(work,&f->material),
        .final={.material=&f->material,.physical=&f->physical}};
    return status;
}
static XrXirStatus class_storage_range_compare(ClassStorageRangeFixture *f) {
    RootParameterMark held=rp_mark();uint64_t live=rp_stats(f->owner.work).live_bytes;
    EffectInvocationClassFact *actual=NULL,*expected=NULL;uint32_t count=0,wanted=0;
    XrXirStatus status=effect_invocation_class_storage(&f->owner,0,&actual,&count);
    XrXirStatus old=class_storage_random_oracle(&f->owner,0,&expected,&wanted);
    CHECK(status==old);
    if(status==XR_XIR_OK){CHECK(count==wanted && count==f->membership.classes);
        uint32_t at=0;
        for(uint32_t r=0;r<f->membership.count;++r)if(f->records[r].mask&EFFECT_PHYSICAL_CLASS){
            CHECK(at<count && actual[at].value==f->records[r].value && actual[at].value==expected[at].value &&
                !actual[at].mask && !actual[at].rank && !expected[at].mask && !expected[at].rank);++at;}
        CHECK(at==count);
    }else CHECK(!actual && !expected && !count && !wanted);
    xr_compile_resources_free(actual);xr_compile_resources_free(expected);rp_balanced(held);
    CHECK(rp_stats(f->owner.work).live_bytes==live);return status;
}
static XrXirStatus class_storage_range_delegate(const EffectBodyRequest *request,uint32_t function,
    uint32_t value,XrXirType *output) {
    if(class_storage_range_callbacks++==class_storage_range_refused)return XR_XIR_IO;
    return effect_body_owned_type(request,function,value,output);
}
static void class_storage_range_reader_controls(ClassStorageRangeFixture *f) {
    EffectBodyRequest original=f->owner.body;f->owner.body.type=class_storage_range_delegate;
    for(size_t site=0;site<f->membership.classes;++site){
        RootParameterMark held=rp_mark();class_storage_range_refused=site;
        for(unsigned reader=0;reader<2;++reader){EffectInvocationClassFact *facts=NULL;uint32_t count=0;
            class_storage_range_callbacks=0;
            CHECK((reader?class_storage_random_oracle(&f->owner,0,&facts,&count):
                effect_invocation_class_storage(&f->owner,0,&facts,&count))==XR_XIR_IO &&
                !facts && !count && class_storage_range_callbacks==site+1);rp_balanced(held);}
    }
    class_storage_range_refused=0;uint32_t saved=f->records[0].value;f->records[0].value=f->membership.values;
    EffectInvocationClassFact *facts=NULL;uint32_t count=0;class_storage_range_callbacks=0;
    CHECK(effect_invocation_class_storage(&f->owner,0,&facts,&count)==XR_XIR_IO &&
        !facts && !count && class_storage_range_callbacks==1);f->records[0].value=saved;
    class_storage_range_refused=SIZE_MAX;f->owner.body=original;
}
static void class_storage_range_faults(ClassStorageRangeFixture *f) {
    CHECK(f->function.run_count>=3);RootParameterMark held=rp_mark();
    for(unsigned fault=0;fault<12;++fault){
        EffectBodyRun *runs=f->function.runs;EffectBodyRun saved=runs[1],first=runs[0],last=runs[f->function.run_count-1];
        uint32_t run_count=f->function.run_count,capacity=f->function.run_capacity;
        if(!fault)++f->function.run_count;else if(fault==1)f->function.run_count=0;
        else if(fault==2)++runs[0].first;else if(fault==3)++runs[1].first;
        else if(fault==4)--runs[f->function.run_count-1].count;
        else if(fault==5)runs[1].mask|=UINT32_C(1)<<14;
        else if(fault==6)runs[1].count=1;
        else if(fault==7)runs[1].base.op=XR_XIR_INVALID;
        else if(fault==8)runs[1].literal_tail[0].op=XR_XIR_OP_COUNT;
        else if(fault==9){runs[1].mask=UINT32_C(1)<<1;runs[1].delta[1]=0;}
        else if(fault==10)runs[f->function.run_count-1].first=UINT32_MAX;
        else f->function.run_capacity=0;
        EffectInvocationClassFact *facts=NULL;uint32_t count=0;
        CHECK(effect_invocation_class_storage(&f->owner,0,&facts,&count)==XR_XIR_BAD_STRUCTURE && !facts && !count);
        f->function.run_count=run_count;f->function.run_capacity=capacity;runs[0]=first;runs[1]=saved;runs[run_count-1]=last;rp_balanced(held);
    }
    for(unsigned fault=0;fault<8;++fault){
        EffectPhysicalFunction membership=f->membership;EffectPhysicalRecord first=f->records[0],third=f->records[2];
        if(!fault)++f->membership.values;else if(fault==1)++f->membership.count;
        else if(fault==2)++f->membership.classes;else if(fault==3)--f->membership.classes;
        else if(fault==4)f->records[2].value=0;else if(fault==5)f->records[0].value=1;
        else if(fault==6)f->records[2].value=f->membership.values;else f->records[2].mask=0;
        EffectInvocationClassFact *facts=NULL;uint32_t count=0;
        CHECK(effect_invocation_class_storage(&f->owner,0,&facts,&count)==XR_XIR_BAD_STRUCTURE && !facts && !count);
        f->membership=membership;f->records[0]=first;f->records[2]=third;rp_balanced(held);
    }
    EffectBodyRequest original=f->owner.body;EffectPhysicalDomain *physical=f->material.physical;
    f->material.physical=NULL;EffectInvocationClassFact *facts=NULL;uint32_t count=0;
    CHECK(effect_invocation_class_storage(&f->owner,0,&facts,&count)==XR_XIR_BAD_STRUCTURE && !facts && !count);f->material.physical=physical;
    ++f->owner.body.domain.function_count;CHECK(effect_invocation_class_storage(&f->owner,0,&facts,&count)==XR_XIR_BAD_STRUCTURE && !facts && !count);f->owner.body=original;
    XrXirCompileContext foreign=rp_owner(rp_caps());uint64_t foreign_live=rp_stats(&foreign).live_bytes;
    f->owner.body.read.owner=&foreign;CHECK(effect_invocation_class_storage(&f->owner,0,&facts,&count)==XR_XIR_BAD_STRUCTURE && !facts && !count);
    f->owner.body=original;rp_owner_free(&foreign,foreign_live);rp_balanced(held);
}
static void class_storage_range_physical_controls(void) {
    RootParameterMark held=rp_mark();
    for(unsigned layout=0;layout<3;++layout)for(uint32_t count=0;count<=24;++count){
        XrXirCompileContext work=rp_owner(rp_caps());uint64_t baseline=rp_stats(&work).live_bytes;ClassStorageRangeFixture f;
        CHECK(class_storage_range_fixture(&work,&f,count,layout)==XR_XIR_OK);
        CHECK(class_storage_range_compare(&f)==XR_XIR_OK);
        if(layout==2 && count==24){
            class_storage_range_faults(&f);class_storage_range_reader_controls(&f);
            for(uint32_t r=0;r<f.function.run_count;++r)for(uint32_t i=0;i<f.function.runs[r].count;++i){
                XrXirInstruction *row=i?&f.function.runs[r].literal_tail[i-1]:&f.function.runs[r].base;
                XrXirInstruction saved=*row;
                for(unsigned field=0;field<10;++field){conditional_compact_change(row,field);
                    (void)class_storage_range_compare(&f);*row=saved;++class_storage_range_mutations;}
            }
        }
        xr_compile_resources_free(f.function.runs);rp_owner_free(&work,baseline);rp_balanced(held);
    }
}
static XrXirStatus class_storage_range_work_case(const XrXirCompileContext *work,uint64_t cut,bool oom) {
    ClassStorageRangeFixture f;XrXirStatus status=class_storage_range_fixture(work,&f,24,2);
    if(status!=XR_XIR_OK){xr_compile_resources_free(f.function.runs);return status;}
    RootParameterMark held=rp_mark();uint64_t entry=rp_stats(work).work;
    if(cut!=UINT64_MAX){CHECK(cut<=work->resources->limits.work-entry);work->resources->limits.work=entry+cut;}
    size_t attempts=rp_attempts;if(oom){rp_fail_at=attempts;rp_injected=false;}
    EffectInvocationClassFact *facts=NULL;uint32_t count=0;
    status=effect_invocation_class_storage(&f.owner,0,&facts,&count);
    if(cut==UINT64_MAX && !oom)class_storage_range_work=rp_stats(work).work-entry;
    if(oom){rp_fail_at=SIZE_MAX;CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts==attempts+1 && !facts && !count);}
    if(status!=XR_XIR_OK){CHECK(!facts && !count);rp_balanced(held);
        if(status==XR_XIR_BUDGET){CHECK(effect_invocation_class_storage(&f.owner,0,&facts,&count)==XR_XIR_BUDGET && !facts && !count);rp_balanced(held);}}
    else {CHECK(count==f.membership.classes);xr_compile_resources_free(facts);rp_balanced(held);}
    xr_compile_resources_free(f.function.runs);return status;
}
static XrXirStatus class_storage_range_genuine(const XrXirCompileContext *work,uint32_t mode,bool compare) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,mode);XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(work,&fixture.source.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK){EffectInvocationCertificate *certificate=effects->invocations;EffectInvocationWorkspace replay={0};
        status=effect_invocation_read_open(work,effects,certificate->declared,certificate->final,&replay);
        if(compare)for(uint32_t f=0;status==XR_XIR_OK && f<replay.body.domain.function_count;++f){
            RootParameterMark held=rp_mark();EffectInvocationClassFact *actual=NULL,*expected=NULL;uint32_t count=0,wanted=0;
            status=effect_invocation_class_storage(&replay,f,&actual,&count);
            if(status==XR_XIR_OK)status=class_storage_random_oracle(&replay,f,&expected,&wanted);
            if(status==XR_XIR_OK){CHECK(count==wanted);for(uint32_t i=0;i<count;++i)
                CHECK(actual[i].value==expected[i].value && !actual[i].mask && !actual[i].rank && !expected[i].mask && !expected[i].rank);}
            xr_compile_resources_free(actual);xr_compile_resources_free(expected);rp_balanced(held);
        }
        EffectBodyRequest body=effect_body_owned_request(work,certificate->body);
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);
        if(status==XR_XIR_OK)status=effect_invocation_publication_match(work,certificate,&certificate->final->summary);
        effect_invocation_read_close(&replay);
    }
    xr_xir_compile_effects_free(effects);return status;
}
static void invocation_class_storage_range_controls(void) {
    class_storage_range_physical_controls();RootParameterMark held=rp_mark();
    XrXirCompileContext work=rp_owner(rp_caps());uint64_t baseline=rp_stats(&work).live_bytes;
    CHECK(class_storage_range_work_case(&work,UINT64_MAX,false)==XR_XIR_OK);rp_owner_free(&work,baseline);rp_balanced(held);
    CHECK(class_storage_range_work && class_storage_range_mutations==240);
    for(uint64_t cut=0;cut<=class_storage_range_work;++cut){work=rp_owner(rp_caps());baseline=rp_stats(&work).live_bytes;
        CHECK(class_storage_range_work_case(&work,cut,false)==(cut==class_storage_range_work?XR_XIR_OK:XR_XIR_BUDGET));
        rp_owner_free(&work,baseline);rp_balanced(held);++class_storage_range_cuts;}
    work=rp_owner(rp_caps());baseline=rp_stats(&work).live_bytes;
    CHECK(class_storage_range_work_case(&work,UINT64_MAX,true)==XR_XIR_OUT_OF_MEMORY);rp_owner_free(&work,baseline);rp_balanced(held);
    for(uint32_t mode=0;mode<7;++mode){work=rp_owner(rp_caps());baseline=rp_stats(&work).live_bytes;
        CHECK(class_storage_range_genuine(&work,mode,true)==XR_XIR_OK);rp_owner_free(&work,baseline);rp_balanced(held);}
    work=rp_owner(rp_caps());baseline=rp_stats(&work).live_bytes;
    CHECK(class_storage_range_genuine(&work,6,false)==XR_XIR_OK);XrCompileResourceStats measured=rp_stats(&work);
    rp_owner_free(&work,baseline);rp_balanced(held);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits caps=rp_caps();
        if(!axis)caps.allocated_bytes=measured.allocated_bytes-minus;else if(axis==1)caps.live_bytes=measured.peak_bytes-minus;else caps.work=measured.work-minus;
        work=rp_owner(caps);baseline=rp_stats(&work).live_bytes;
        CHECK(class_storage_range_genuine(&work,6,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&work,baseline);rp_balanced(held);}
    fprintf(stderr,"Class storage ranges: complete frozen random oracle and independent literal membership across75 empty/constant/affine/literal/mixed inputs, all240 physical word mutations,12 canonical directory/payload and8 membership refusals, current binding/foreign ledger and every delegated IO site; actual one allocation OOM, every work unit across%llu cuts and same finite-owner retry without reset, fresh mask/rank and failed output preservation; all7 genuine Class/Cell bodies after producer/Checked death with original independent Class/publication and genuine six axes%llu/%llu/%llu exact/minusone physical0 PASS\n",
        (unsigned long long)class_storage_range_cuts,(unsigned long long)measured.allocated_bytes,
        (unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_CLASS_STORAGE_RANGE_CASES_H
