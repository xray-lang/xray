/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_cell_borrow_cases.h - Genuine current Cell view consumption
 */
#ifndef XIR_INVOCATION_CELL_BORROW_CASES_H
#define XIR_INVOCATION_CELL_BORROW_CASES_H
static size_t invocation_cell_borrow_values,invocation_cell_borrow_callbacks;
static bool invocation_cell_borrow_refused;
static XrXirStatus invocation_cell_borrow_type(const EffectBodyRequest *request,uint32_t function,
    uint32_t value,XrXirType *output) {
    ++invocation_cell_borrow_callbacks;if(invocation_cell_borrow_refused)return XR_XIR_IO;
    return effect_body_owned_type(request,function,value,output);
}
static XrXirStatus invocation_cell_borrow_query(EffectInvocationFlow *flow,uint32_t value,
    uint64_t *row,bool *changed) {
    RootParameterMark physical=rp_mark();size_t attempts=rp_attempts;XrCompileResourceStats before=rp_stats(flow->owner->work);
    EffectInvocationNode *node=&flow->owner->final.nodes[flow->node],held=*node;
    XrXirStatus status=effect_invocation_cell(flow,value,row,changed);
    CHECK(!memcmp(node,&held,sizeof(held)) && rp_attempts==attempts &&
        rp_stats(flow->owner->work).allocated_bytes==before.allocated_bytes && rp_stats(flow->owner->work).live_bytes==before.live_bytes);
    rp_balanced(physical);return status;
}
/* Both receiving formats enter the same semantic projector. Independent
 * literal masks below additionally name the genuine projected Cell facts. */
static void invocation_cell_borrow_compare(EffectInvocationFlow *flow) {
    CHECK(flow->basis.cell_words && flow->basis.cell_words<=8);EffectBodyRequest original=flow->body;
    size_t bytes=(size_t)flow->basis.cell_words*sizeof(uint64_t);
    for(uint32_t value=0;value<flow->values;++value){uint64_t actual[8]={UINT64_C(1)<<40},expected[8]={UINT64_C(1)<<40};
        bool changed=false,expected_changed=false;
        CHECK(invocation_cell_borrow_query(flow,value,actual,&changed)==XR_XIR_OK);
        XrXirCellClassOriginView storage={0};const XrXirCellOriginView *view=NULL;
        CHECK(effect_invocation_class_cell_view(flow,value,&storage,&view)==XR_XIR_OK && view);
        EffectInvocationCellProjection projection={flow,flow->owner->final.nodes[flow->node].input,expected,&expected_changed};
        CHECK(effect_invocation_cell_project(&projection,view,view->intrinsic_mask)==XR_XIR_OK &&
            changed==expected_changed && !memcmp(actual,expected,bytes));
        flow->body.type=invocation_cell_borrow_type;memset(expected,0,sizeof(expected));expected[0]=UINT64_C(1)<<40;expected_changed=false;
        CHECK(invocation_cell_borrow_query(flow,value,expected,&expected_changed)==XR_XIR_OK &&
            changed==expected_changed && !memcmp(actual,expected,bytes));flow->body=original;++invocation_cell_borrow_values;
    }
}
static void invocation_cell_borrow_subject_faults(EffectInvocationFlow *flow,uint32_t place,uint32_t project) {
    XrXirInstruction storage={0};const XrXirInstruction *op=NULL;
    CHECK(effect_invocation_flow_instruction(flow,place,&storage,&op)==XR_XIR_OK && op->op==XR_XIR_OBJECT_PLACE);
    uint32_t at=0;CHECK(effect_invocation_class_find(flow->owner->work,&flow->owner->final.nodes[flow->node],op->args[0],&at)==XR_XIR_OK);
    EffectInvocationClassFact *fact=&flow->owner->final.nodes[flow->node].classes[at],held=*fact;
    for(unsigned fault=0;fault<4;++fault){*fact=held;
        if(!fault){fact->mask=0;fact->rank=0;}else if(fault==1){fact->mask=EFFECT_CLASS_KNOWN;fact->rank=0;}
        else if(fault==2){fact->mask=EFFECT_CLASS_KNOWN|EFFECT_CLASS_ROOT;fact->rank=1;}
        else {fact->mask=EFFECT_CLASS_KNOWN|UINT32_C(8);fact->rank=0;}
        uint64_t row[8]={UINT64_C(1)<<40},before[8];memcpy(before,row,sizeof(row));bool changed=false;
        CHECK(invocation_cell_borrow_query(flow,project,row,&changed)==(!fault?XR_XIR_UNRESOLVED:XR_XIR_BAD_STRUCTURE) &&
            !changed && !memcmp(row,before,sizeof(row)));*fact=held;
    }
    EffectBodyRequest original=flow->body;flow->body.type=invocation_cell_borrow_type;invocation_cell_borrow_refused=true;
    uint64_t row[8]={UINT64_C(1)<<40},before[8];memcpy(before,row,sizeof(row));bool changed=false;
    size_t calls=invocation_cell_borrow_callbacks;
    CHECK(invocation_cell_borrow_query(flow,project,row,&changed)==XR_XIR_IO && !changed &&
        !memcmp(row,before,sizeof(row)) && invocation_cell_borrow_callbacks==calls+1);
    CHECK(invocation_cell_borrow_query(flow,project,row,&changed)==XR_XIR_IO && !changed &&
        !memcmp(row,before,sizeof(row)) && invocation_cell_borrow_callbacks==calls+2);
    invocation_cell_borrow_refused=false;flow->body=original;
}
static void invocation_cell_borrow_work_cuts(EffectInvocationFlow *flow,uint32_t value) {
    const XrXirCompileContext *context=flow->owner->work;uint64_t row[8]={0};bool changed=false;
    uint64_t entry=rp_stats(context).work;CHECK(invocation_cell_borrow_query(flow,value,row,&changed)==XR_XIR_OK);
    uint64_t exact=rp_stats(context).work-entry,limit=context->resources->limits.work;
    for(uint64_t cut=0;cut<=exact;++cut){entry=rp_stats(context).work;CHECK(exact<=limit-entry);
        context->resources->limits.work=entry+cut;memset(row,0,sizeof(row));changed=false;
        CHECK(invocation_cell_borrow_query(flow,value,row,&changed)==(cut==exact?XR_XIR_OK:XR_XIR_BUDGET));
        if(cut<exact)CHECK(invocation_cell_borrow_query(flow,value,row,&changed)==XR_XIR_BUDGET);
        context->resources->limits.work=limit;
    }
}
/* Real source checking and complete effects derivation precede the death
 * of every caller-owned byte. No private literal table supplies authority. */
static XrXirStatus invocation_cell_borrow_run(const XrXirCompileContext *context,uint32_t mode,bool controls) {
    InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,mode);uint32_t project=fixture.project,place=fixture.place;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectInvocationWorkspace replay={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
    if(status==XR_XIR_OK){EffectInvocationCertificate *certificate=effects->invocations;CHECK(certificate && certificate->final);
        status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&replay);
        EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
        for(uint32_t root=3;status==XR_XIR_OK && root<=5;root+=2){
            EffectInvocationFlow flow={.owner=&replay,.node=replay.final.roots[root],.instruction=UINT32_MAX,.precision=true};
            status=effect_invocation_origins_read(&flow,&body);
            if(status==XR_XIR_OK){CHECK(flow.types_ready && replay.class_enabled && flow.basis.cell_words==1);
                uint64_t row[8]={0};bool changed=false;uint32_t value=root==3?project:0;
                status=invocation_cell_borrow_query(&flow,value,row,&changed);
                if(status==XR_XIR_OK){CHECK(changed && row[0]==(root==5?UINT64_C(9):mode<3?UINT64_C(4):UINT64_C(6)));
                    if(controls){invocation_cell_borrow_compare(&flow);if(root==3)invocation_cell_borrow_subject_faults(&flow,place,project);
                        invocation_cell_borrow_work_cuts(&flow,value);}}
            }
            xr_compile_resources_free(flow.memory);
        }
        if(status==XR_XIR_OK)status=effect_invocation_class_verify_read(&replay,&body);
    }
    effect_invocation_read_close(&replay);xr_xir_compile_effects_free(effects);return status;
}
static void invocation_cell_borrow_controls(void) {
    RootParameterMark physical=rp_mark();XrCompileResourceStats measured={0};
    for(uint32_t mode=0;mode<7;++mode){XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
        CHECK(invocation_cell_borrow_run(&context,mode,true)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);}
    XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    CHECK(invocation_cell_borrow_run(&context,6,false)==XR_XIR_OK);measured=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){XrCompileResourceLimits limits=rp_caps();
        if(!axis)limits.allocated_bytes=measured.allocated_bytes-minus;
        else if(axis==1)limits.live_bytes=measured.peak_bytes-minus;else limits.work=measured.work-minus;
        context=rp_owner(limits);baseline=rp_stats(&context).live_bytes;
        CHECK(invocation_cell_borrow_run(&context,6,false)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);}
    CHECK(invocation_cell_borrow_values && invocation_cell_borrow_callbacks);
    fprintf(stderr,"Cell borrowed receivers: all7 genuine Class/Cell certificates after Checked/producer death, two real root contexts and literal pure/UNKNOWN/scoped rows, all%zu actual values match original copied and delegated projections including changed and retained bits, real Class subject bottom/invalid mask/rank and IO retries before row effects, every work unit through exact and same finite-owner retry, no allocation, fresh independent Class verification; whole genuine-fixture six axes%llu/%llu/%llu exact/minusone PASS\n",
        invocation_cell_borrow_values,(unsigned long long)measured.allocated_bytes,(unsigned long long)measured.peak_bytes,(unsigned long long)measured.work);
}
#endif // XIR_INVOCATION_CELL_BORROW_CASES_H
