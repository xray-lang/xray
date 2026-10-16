/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_type_projection_cases.h - Temporary physical field projection
 */
#ifndef XIR_INVOCATION_TYPE_PROJECTION_CASES_H
#define XIR_INVOCATION_TYPE_PROJECTION_CASES_H

static unsigned invocation_type_delegated_reads;
static XrXirStatus invocation_type_delegated(const EffectBodyRequest *request,
    uint32_t function,uint32_t value,XrXirType *output) {
    ++invocation_type_delegated_reads;
    if (value==3) return XR_XIR_BAD_VALUE;
    return effect_body_owned_type(request,function,value,output);
}

static void invocation_type_storage_alignment(void) {
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    EffectPhysicalFunction physical={.parameters=1,.instructions=2,.values=3};
    EffectPhysicalDomain domain={.resources=context.resources,.functions=&physical,.count=1};
    EffectBodyFunction body={0};EffectBodyMaterial material={.resources=context.resources,
        .references=1,.functions=&body};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectInvocationNode node={0};EffectInvocationWorkspace owner={.work=&context,
        .final={.nodes=&node,.count=1,.physical=&domain,.material=&material}};
    EffectBodyFunctionHead head={.parameter_count=1,.instruction_count=2};
    EffectInvocationFlow flow={.owner=&owner,.function=&head,
        .body=effect_body_owned_request(&context,&material)};
    RootParameterMark held=rp_mark();rp_fail_at=rp_attempts;rp_injected=false;
    CHECK(effect_invocation_flow_storage(&flow)==XR_XIR_OUT_OF_MEMORY && rp_injected && !flow.memory);
    rp_fail_at=SIZE_MAX;rp_balanced(held);
    CHECK(effect_invocation_flow_storage(&flow)==XR_XIR_OK && flow.types && flow.instructions &&
        (uintptr_t)flow.instructions%_Alignof(XrXirInstruction)==0);
    xr_compile_resources_free(flow.memory);rp_owner_free(&context,baseline);rp_balanced(initial);
}

/* These owned physical rows are not an admitted program. Independent literal
 * types and malformed storage exercise the projection without ROOT authority. */
static void invocation_type_projection_literals(void) {
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    const XrXirType expected[]={XR_XIR_BOOL,XR_XIR_STRING,XR_XIR_I64,XR_XIR_I64,
        XR_XIR_I64,XR_XIR_I64,XR_XIR_F64,XR_XIR_F64,XR_XIR_F64,XR_XIR_F64};
    XrXirInstruction ops[8]={0};
    for (uint32_t i=0;i<8;++i) ops[i]=(XrXirInstruction){
        .op=i<4?XR_XIR_CONST_INT:XR_XIR_CONST_FLOAT,.type=expected[i+2],.immediate=INT64_MIN+i,
        .args={i+9,UINT32_MAX-i},.targets={i*3,43-i},.type_arguments={300+i,700-i}};
    XrXirFunction input={.instructions=ops,.instruction_count=8};
    EffectBodyFunction body={.parameters=expected,.parameter_count=2,.instruction_count=8};
    CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK && body.run_count==2);
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&body};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectInvocationNode node={.body=0};
    EffectInvocationWorkspace owner={.work=&context,.final={.material=&material,.nodes=&node,.count=1}};
    EffectBodyFunctionHead head={.parameters=expected,.parameter_count=2,.instruction_count=8};
    XrXirType projected[10]={0};XrXirInstruction projected_ops[8]={0};
    EffectInvocationFlow flow={.owner=&owner,.function=&head,.values=10,.types=projected,.instructions=projected_ops,
        .body=effect_body_owned_request(&context,&material)};
    CHECK(effect_invocation_flow_types(&flow)==XR_XIR_OK && flow.types_ready);
    for(uint32_t i=0;i<8;++i){XrXirInstruction storage;const XrXirInstruction *row=NULL;
        CHECK(effect_invocation_flow_instruction(&flow,i,&storage,&row)==XR_XIR_OK && row==&projected_ops[i] &&
            row->op==(i<4?XR_XIR_CONST_INT:XR_XIR_CONST_FLOAT) && row->type==expected[i+2] &&
            row->immediate==INT64_MIN+i && row->args[0]==i+9 && row->args[1]==UINT32_MAX-i &&
            row->targets[0]==i*3 && row->targets[1]==43-i &&
            row->type_arguments[0]==300+i && row->type_arguments[1]==700-i);}
    for (uint32_t i=0;i<10;++i) { XrXirType type=XR_XIR_UNIT;
        CHECK(projected[i]==expected[i] && effect_invocation_flow_type(&flow,i,&type)==XR_XIR_OK && type==expected[i]); }
    XrXirType output=XR_XIR_U8;
    CHECK(effect_invocation_flow_type(&flow,10,&output)==XR_XIR_BAD_STRUCTURE && output==XR_XIR_U8);
    CHECK(effect_invocation_flow_types(&flow)==XR_XIR_BAD_STRUCTURE);
    uint32_t first=body.runs[1].first,count=body.runs[1].count;
    for (unsigned mode=0;mode<3;++mode) {
        flow.types_ready=false;
        if (!mode) body.runs[1].first=first+1;
        else if (mode==1) body.runs[1].count=0;
        else flow.body.domain.function_count=2;
        CHECK(effect_invocation_flow_types(&flow)==XR_XIR_BAD_STRUCTURE && !flow.types_ready);
        body.runs[1].first=first;body.runs[1].count=count;
        flow.body=effect_body_owned_request(&context,&material);
        CHECK(effect_invocation_flow_types(&flow)==XR_XIR_OK && flow.types_ready);
    }
    flow.body.type=invocation_type_delegated;
    invocation_type_delegated_reads=0;output=XR_XIR_U8;
    CHECK(effect_invocation_flow_type(&flow,3,&output)==XR_XIR_BAD_VALUE && output==XR_XIR_U8 &&
        invocation_type_delegated_reads==1);
    flow.types_ready=false;
    CHECK(effect_invocation_flow_types(&flow)==XR_XIR_OK && !flow.types_ready);
    invocation_type_delegated_reads=0;output=XR_XIR_U8;
    CHECK(effect_invocation_flow_type(&flow,3,&output)==XR_XIR_BAD_VALUE && output==XR_XIR_U8 &&
        invocation_type_delegated_reads==1);
    CHECK(effect_invocation_flow_type(&flow,8,&output)==XR_XIR_OK && output==XR_XIR_F64 &&
        invocation_type_delegated_reads==2);
    xr_compile_resources_free(body.runs);rp_owner_free(&context,baseline);rp_balanced(initial);
    invocation_type_storage_alignment();
}
#endif // XIR_INVOCATION_TYPE_PROJECTION_CASES_H
