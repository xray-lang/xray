/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_native_barrier_cases.h - Actual synchronous barrier inputs
 */
#ifndef XIR_INVOCATION_NATIVE_BARRIER_CASES_H
#define XIR_INVOCATION_NATIVE_BARRIER_CASES_H
typedef struct NativeBarrierFixture {
    XrXirTypeNode nodes[4];XrXirType parameters[4],types[7];
    EffectBodyRun run;EffectBodyFunction body;EffectBodyMaterial material;
    EffectBodyFunctionHead head;EffectInvocationWorkspace owner;EffectInvocationNode node;
    EffectPhysicalFunction physical;EffectPhysicalRecord records[3];
    EffectInvocationValueBound bounds[3];EffectInvocationFunctionBounds function_bounds;
    EffectInvocationDeclaredBounds declared;EffectInvocationFlow flow;
    const XrXirInstruction *references[3];uint32_t operands[4],indices[7];
    uint64_t rows[3],flags[2];
} NativeBarrierFixture;
static size_t native_barrier_reads,native_barrier_refused=SIZE_MAX;
static size_t native_barrier_genuine_rows,native_barrier_genuine_callbacks;
static XrXirStatus native_barrier_type(const EffectBodyRequest *request,uint32_t function,
    uint32_t value,XrXirType *output) {
    if(native_barrier_reads++==native_barrier_refused)return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_type(request,function,value,output);
}
static XrXirStatus native_barrier_instruction(const EffectBodyRequest *request,uint32_t function,
    uint32_t ordinal,XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    if(native_barrier_reads++==native_barrier_refused)return XR_XIR_BAD_STRUCTURE;
    return effect_body_owned_instruction(request,function,ordinal,storage,output,fields);
}
static void native_barrier_clear(EffectInvocationFlow *flow) {
    size_t rows=(size_t)flow->row_count*flow->basis.fn_words,flags=((size_t)flow->values+63)/64;
    if(rows)memset(flow->rows,0,rows*sizeof(*flow->rows));
    if(flags)memset(flow->fixed,0,2*flags*sizeof(*flow->fixed));flow->changed=false;
}
static XrXirStatus native_barrier_execute(EffectInvocationFlow *flow) {
    RootParameterMark physical=rp_mark();size_t attempts=rp_attempts;
    XrCompileResourceStats before=rp_stats(flow->owner->work);XrXirStatus status=effect_invocation_barriers(flow);
    CHECK(rp_attempts==attempts && rp_stats(flow->owner->work).allocated_bytes==before.allocated_bytes &&
        rp_stats(flow->owner->work).live_bytes==before.live_bytes);rp_balanced(physical);return status;
}
/* This small physical probe supplies no Checked artifact or permission.
 * Literal expected bits precede the independent delegated-reader oracle. */
static void native_barrier_fixture(const XrXirCompileContext *context,NativeBarrierFixture *f) {
    memset(f,0,sizeof(*f));
    for(uint32_t i=0;i<3;++i){f->nodes[i]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_UNIT,
            .flags=i==1?XR_XIR_CALLABLE_ROOT_REQUIRED:i==2?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_NONE};
        f->parameters[i]=(XrXirType)(256+i);f->bounds[i]=(EffectInvocationValueBound){.value=i,.callable=true,
            .mask=i==1?XR_XIR_CALLABLE_ROOT_REQUIRED:i==2?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0};
        f->records[i]=(EffectPhysicalRecord){.value=4+i,.mask=EFFECT_PHYSICAL_BARRIER};}
    f->parameters[3]=XR_XIR_I64;f->nodes[3]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CELL,.element=(XrXirType)257};
    f->run=(EffectBodyRun){.count=3,.mask=EFFECT_BODY_LITERAL_ROWS,
        .base={.op=XR_XIR_ARRAY_NEW,.type=XR_XIR_UNIT,.args={0,4}}};
    f->run.literal_tail[0]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)259,.args={1}};
    f->run.literal_tail[1]=(XrXirInstruction){.op=XR_XIR_CELL_WRITE,.type=XR_XIR_UNIT,.args={5,2}};
    for(uint32_t i=0;i<4;++i){f->operands[i]=i;f->types[i]=f->parameters[i];}
    for(uint32_t i=0;i<3;++i){f->references[i]=i?&f->run.literal_tail[i-1]:&f->run.base;f->types[4+i]=f->references[i]->type;}
    for(uint32_t i=0;i<7;++i)f->indices[i]=i<3?i:UINT32_MAX;
    f->body=(EffectBodyFunction){.parameters=f->parameters,.parameter_count=4,.instruction_count=3,
        .operands=f->operands,.operand_count=4,.runs=&f->run,.run_count=1,.run_capacity=1};
    f->material=(EffectBodyMaterial){.resources=context->resources,.references=1,.functions=&f->body};
    f->material.terms.types=(XrXirTypes){f->nodes,4,NULL,NULL};
    f->material.domain=(XirMetadataDomain){.types=&f->material.terms.types,.function_count=1};
    f->head=(EffectBodyFunctionHead){.parameters=f->parameters,.parameter_count=4,.instruction_count=3,
        .operands=f->operands,.operand_count=4};
    f->function_bounds=(EffectInvocationFunctionBounds){.values=7,.count=3,.bounds=f->bounds};
    f->declared=(EffectInvocationDeclaredBounds){.resources=context->resources,.count=1,.capacity=1,.functions=&f->function_bounds};
    f->physical=(EffectPhysicalFunction){.values=7,.parameters=4,.instructions=3,.rows=3,.records=f->records,.count=3,.capacity=3};
    f->owner=(EffectInvocationWorkspace){.work=context,.declared=&f->declared,
        .final={.material=&f->material,.nodes=&f->node,.count=1,.site_count=2}};
    f->flow=(EffectInvocationFlow){.owner=&f->owner,.function=&f->head,.values=7,.types=f->types,
        .instruction_refs=f->references,.physical=&f->physical,.row_count=3,.row_indices=f->indices,
        .rows=f->rows,.fixed=f->flags,.certified=f->flags+1,.basis={.fn_words=1,.origin_words=1,.parameters=4},
        .body=effect_body_owned_request(context,&f->material),.types_ready=true};
}
/* The old callbacks replay every original physical row. Complete state,
 * including changed and both flags, must agree after the same operation. */
static void native_barrier_compare(EffectInvocationFlow *flow,bool refusals) {
    size_t rows=(size_t)flow->row_count*flow->basis.fn_words,flags=((size_t)flow->values+63)/64;
    CHECK(rows<=128 && flags<=2);uint64_t original_rows[128],original_flags[4],expected_rows[128],expected_flags[4];
    if(rows)memcpy(original_rows,flow->rows,rows*sizeof(uint64_t));
    if(flags)memcpy(original_flags,flow->fixed,2*flags*sizeof(uint64_t));
    bool changed=flow->changed;EffectBodyRequest original=flow->body;native_barrier_clear(flow);
    CHECK(native_barrier_execute(flow)==XR_XIR_OK);bool expected_changed=flow->changed;
    if(rows)memcpy(expected_rows,flow->rows,rows*sizeof(uint64_t));
    if(flags)memcpy(expected_flags,flow->fixed,2*flags*sizeof(uint64_t));
    for(unsigned reader=0;reader<3;++reader){flow->body=original;
        if(reader!=1)flow->body.type=native_barrier_type;
        if(reader!=0)flow->body.instruction=native_barrier_instruction;
        native_barrier_clear(flow);native_barrier_reads=0;native_barrier_refused=SIZE_MAX;
        CHECK(native_barrier_execute(flow)==XR_XIR_OK && flow->changed==expected_changed);
        CHECK(!rows || !memcmp(expected_rows,flow->rows,rows*sizeof(uint64_t)));
        CHECK(!flags || !memcmp(expected_flags,flow->fixed,2*flags*sizeof(uint64_t)));
        size_t sites=native_barrier_reads;native_barrier_genuine_callbacks+=sites;
        if(refusals)for(size_t at=0;at<sites;++at){native_barrier_clear(flow);native_barrier_reads=0;native_barrier_refused=at;
            CHECK(native_barrier_execute(flow)==XR_XIR_BAD_STRUCTURE && native_barrier_reads==at+1);
            native_barrier_clear(flow);native_barrier_reads=0;
            CHECK(native_barrier_execute(flow)==XR_XIR_BAD_STRUCTURE && native_barrier_reads==at+1);}
    }
    native_barrier_refused=SIZE_MAX;flow->body=original;flow->changed=changed;
    if(rows)memcpy(flow->rows,original_rows,rows*sizeof(uint64_t));
    if(flags)memcpy(flow->fixed,original_flags,2*flags*sizeof(uint64_t));
}
static void native_barrier_faults(const XrXirCompileContext *context) {
    NativeBarrierFixture f;
    for(uint32_t i=0;i<3;++i)for(unsigned bound=0;bound<2;++bound){native_barrier_fixture(context,&f);
        XrXirInstruction *row=i?&f.run.literal_tail[i-1]:&f.run.base;row->op=bound?XR_XIR_OP_COUNT:XR_XIR_INVALID;
        CHECK(native_barrier_execute(&f.flow)==XR_XIR_BAD_STRUCTURE);}
    for(unsigned fault=0;fault<23;++fault){native_barrier_fixture(context,&f);XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        if(!fault)f.owner.final.material=NULL;else if(fault==1)f.flow.node=1;
        else if(fault==2)f.flow.physical=NULL;else if(fault==3)f.flow.function=NULL;
        else if(fault==4)++f.flow.values;else if(fault==5)++f.head.parameter_count;
        else if(fault==6)f.flow.types=NULL;else if(fault==7)f.flow.instruction_refs=NULL;
        else if(fault==8)f.references[0]=NULL;else if(fault==9)f.flow.body.domain.types=NULL;
        else if(fault==10)++f.flow.body.domain.function_count;
        else if(fault==11)f.records[0].value=3;else if(fault==12)f.records[0].value=7;
        else if(fault==13)f.head.operands=NULL;else if(fault==14)f.run.base.args[0]=5;
        else if(fault==15)f.run.base.args[1]=5;
        else if(fault==16){f.operands[2]=7;expected=XR_XIR_BAD_VALUE;}
        else if(fault==17){f.run.literal_tail[0].args[0]=7;expected=XR_XIR_BAD_VALUE;}
        else if(fault==18){f.run.literal_tail[0].op=XR_XIR_SLOT_INIT;}
        else if(fault==19){f.flow.fixed=NULL;}
        else if(fault==20){f.types[3]=(XrXirType)256;expected=XR_XIR_BAD_TYPE;}
        else if(fault==21){f.bounds[0].callable=false;expected=XR_XIR_BAD_TYPE;}
        else {f.bounds[0].mask=UINT32_MAX;expected=XR_XIR_BAD_TYPE;}
        CHECK(native_barrier_execute(&f.flow)==expected);
    }
    native_barrier_fixture(context,&f);XrXirCompileContext foreign=rp_owner(rp_caps());
    XrCompileResourceStats before=rp_stats(&foreign);size_t attempts=rp_attempts;f.flow.body.read.owner=&foreign;
    CHECK(native_barrier_execute(&f.flow)==XR_XIR_BAD_STRUCTURE && rp_attempts==attempts &&
        rp_stats(&foreign).work==before.work && rp_stats(&foreign).allocated_bytes==before.allocated_bytes);
    rp_owner_free(&foreign,before.live_bytes);
    /* Scalar changes are consumed now. A prior callable observation cannot
     * supply an opaque origin when the current type word is scalar. */
    native_barrier_fixture(context,&f);f.types[1]=XR_XIR_I64;f.parameters[1]=XR_XIR_I64;
    CHECK(native_barrier_execute(&f.flow)==XR_XIR_OK && f.rows[1]==0);native_barrier_compare(&f.flow,true);
    native_barrier_fixture(context,&f);f.flow.body.type=native_barrier_type;native_barrier_reads=0;
    native_barrier_refused=0;CHECK(effect_invocation_fixed(&f.flow,0)==XR_XIR_BAD_STRUCTURE && native_barrier_reads==1 &&
        f.flags[0]==1 && f.flow.changed);native_barrier_refused=SIZE_MAX;
    CHECK(effect_invocation_opaque(&f.flow,7,false)==XR_XIR_BAD_VALUE && native_barrier_reads==1);
}
/* Fresh independent replay uses a real complete Class/Cell certificate
 * after the caller's bytes and Checked producer have both died. */
static XrXirStatus native_barrier_genuine(const XrXirCompileContext *context,bool controls) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,6);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectInvocationWorkspace replay={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK){EffectInvocationCertificate *certificate=effects->invocations;
        CHECK(certificate && certificate->final);status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        for(uint32_t n=0;status==XR_XIR_OK && n<replay.final.count;++n){
            EffectInvocationFlow flow={.owner=&replay,.node=n,.instruction=UINT32_MAX,.precision=true};
            status=effect_invocation_origins_read(&flow,&body);
            if(status==XR_XIR_OK && controls){CHECK(flow.types_ready);native_barrier_compare(&flow,true);
                native_barrier_genuine_rows+=flow.function->instruction_count;}
            xr_compile_resources_free(flow.memory);
        }
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);
    }
    effect_invocation_read_close(&replay);xr_xir_compile_effects_free(effects);return status;
}
static void invocation_native_barrier_controls(void) {
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    NativeBarrierFixture f;native_barrier_fixture(&context,&f);CHECK(native_barrier_execute(&f.flow)==XR_XIR_OK);
    uint64_t exact=rp_stats(&context).work;
    CHECK(f.rows[0]==UINT64_C(64) && f.rows[1]==UINT64_C(192) && f.rows[2]==UINT64_C(320) &&
        f.flags[0]==UINT64_C(47) && !f.flags[1] && f.flow.changed);
    native_barrier_compare(&f.flow,true);native_barrier_faults(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    for(uint64_t cap=1;cap<=exact;++cap){XrCompileResourceLimits limits=rp_caps();limits.work=cap;context=rp_owner(limits);
        baseline=rp_stats(&context).live_bytes;native_barrier_fixture(&context,&f);
        CHECK(native_barrier_execute(&f.flow)==(cap==exact?XR_XIR_OK:XR_XIR_BUDGET));
        if(cap<exact){native_barrier_clear(&f.flow);CHECK(native_barrier_execute(&f.flow)==XR_XIR_BUDGET);}
        rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;CHECK(native_barrier_genuine(&context,false)==XR_XIR_OK);
    XrCompileResourceStats measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;CHECK(native_barrier_genuine(&context,true)==XR_XIR_OK);
    rp_owner_free(&context,baseline);rp_balanced(physical);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits limits=rp_caps();
        if(!axis)limits.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(native_barrier_genuine(&context,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    CHECK(native_barrier_genuine_rows && native_barrier_genuine_callbacks);
    fprintf(stderr,"Native barriers: literal fixed/ROOT/UNKNOWN oracle, complete rows/flags/changed versus original delegated type/instruction reads, all callback IO retries,23 view/domain/operand/bound faults and every invalid/upper opcode, every work unit through%llu and same finite-owner retry, no allocation; genuine Class/Cell certificate after Checked/producer death fresh origins over%zu rows and%zu callbacks plus independent Class verification, whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        (unsigned long long)exact,native_barrier_genuine_rows,native_barrier_genuine_callbacks,
        (unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_NATIVE_BARRIER_CASES_H
