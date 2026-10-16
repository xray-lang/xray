/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_write_index_cases.h - All physical writes remain ordered and checked
 */
#ifndef XIR_INVOCATION_WRITE_INDEX_CASES_H
#define XIR_INVOCATION_WRITE_INDEX_CASES_H
static unsigned invocation_write_delegated_reads;
static XrXirStatus invocation_write_delegated(const EffectBodyRequest *request,uint32_t f,
    uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output,EffectBodyInstructionFields *fields) {
    (void)request;(void)f;(void)i;(void)storage;(void)output;(void)fields;
    ++invocation_write_delegated_reads;return XR_XIR_BAD_VALUE;
}

/* Literal physical rows and Class-source inputs exercise the private consumer;
 * they do not claim a checked body or grant a Class publication permission. */
static void invocation_write_index_literals(void) {
    RootParameterMark initial=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
    uint64_t baseline=rp_stats(&context).live_bytes;
    XrXirNominalDeclaration declaration={.kind=XR_XIR_NOMINAL_CLASS};
    XrXirNominalTable nominals={&declaration,1,NULL};
    XrXirTypeNode type={.kind=XR_XIR_TYPE_NOMINAL};
    XrXirType types[]={256,XR_XIR_UNIT,256,XR_XIR_UNIT,XR_XIR_UNIT,XR_XIR_UNIT};
    XrXirInstruction rows[6]={{.op=XR_XIR_CLASS_NEW,.type=256},
        {.op=XR_XIR_LOCAL_WRITE,.args={5,0}}, {.op=XR_XIR_CLASS_NEW,.type=256},
        {.op=XR_XIR_OWNED_LOCAL_WRITE,.args={5,2}}, {.op=XR_XIR_SCALAR_LOCAL_WRITE,.args={4,0}},
        {.op=XR_XIR_RETURN}};
    EffectBodyFunction body={.instruction_count=6};
    EffectBodyMaterial material={.resources=context.resources,.references=1,.functions=&body};
    material.terms.types=(XrXirTypes){.nodes=&type,.count=1,.nominals=&nominals};
    material.domain=(XirMetadataDomain){.types=&material.terms.types,.function_count=1};
    EffectInvocationClassFact facts[]={{0,EFFECT_CLASS_KNOWN,1},{2,EFFECT_CLASS_KNOWN|EFFECT_CLASS_ROOT,0}};
    EffectInvocationNode node={.instruction_count=6,.classes=facts,.class_count=2};
    EffectInvocationWorkspace owner={.work=&context,.final={.nodes=&node,.count=1,.material=&material}};
    EffectBodyFunctionHead head={.instruction_count=6};uint32_t heads[6],next[6];
    EffectInvocationFlow flow={.owner=&owner,.function=&head,.values=6,.types=types,
        .instructions=rows,.write_heads=heads,.write_next=next,.types_ready=true,
        .body=effect_body_owned_request(&context,&material)};
    CHECK(effect_invocation_flow_write_index(&flow)==XR_XIR_OK && heads[5]==1 && next[1]==3 &&
        next[3]==UINT32_MAX && heads[4]==4 && next[4]==UINT32_MAX && heads[0]==UINT32_MAX);
    EffectInvocationClassSources sources={.complete=true};
    CHECK(effect_invocation_class_writes(&flow,5,&sources)==XR_XIR_OK && sources.complete &&
        sources.mask==(EFFECT_CLASS_KNOWN|EFFECT_CLASS_ROOT) && sources.maximum==1);
    for(unsigned mode=0;mode<3;++mode){
        if(!mode)next[1]=1;
        else if(mode==1)next[1]=6;
        else heads[5]=2;
        sources=(EffectInvocationClassSources){.complete=true};
        CHECK(effect_invocation_class_writes(&flow,5,&sources)==XR_XIR_BAD_STRUCTURE);
        CHECK(effect_invocation_flow_write_index(&flow)==XR_XIR_OK);
    }
    flow.body.instruction=invocation_write_delegated;invocation_write_delegated_reads=0;
    sources=(EffectInvocationClassSources){.complete=true};
    CHECK(effect_invocation_class_writes(&flow,5,&sources)==XR_XIR_BAD_VALUE && invocation_write_delegated_reads==1);
    rp_owner_free(&context,baseline);rp_balanced(initial);
}
#endif // XIR_INVOCATION_WRITE_INDEX_CASES_H
