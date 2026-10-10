/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_runtime_edges_cases.h - Literal owned execution edge matching
 */
typedef struct InvocationRuntimeProbe {
    const XrXirCompileContext *context;
    uint32_t cell;
} InvocationRuntimeProbe;
static bool invocation_runtime_work(void *owner,uint64_t work) {
    return xir_compile_work(((InvocationRuntimeProbe *)owner)->context,work);
}
static uint32_t invocation_runtime_cell(void *owner,uint32_t parameter) {
    CHECK(parameter==0);return ((InvocationRuntimeProbe *)owner)->cell;
}
static XrXirStatus conditional_invocation_runtime_edge(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,mode);
    XrXirFunction functions[5];XrXirFunctionIdentity identities[5];
    memcpy(functions,fixture.functions,sizeof(fixture.functions));
    memcpy(identities,fixture.identities,sizeof(fixture.identities));identities[4]=(XrXirFunctionIdentity){0};
    XrXirInstruction instructions[5]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42},
        {.op=XR_XIR_CELL_NEW,.type=fixture.cell},
        {.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=2},
        {.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2,.args={0,1}},
        {.op=XR_XIR_RETURN,.args={3,0}}};
    uint32_t actual=1;XrXirBlock block={.count=5};uint32_t producer_instruction=2,call_instruction=3;
    if (mode) {
        instructions[0]=(XrXirInstruction){.op=XR_XIR_SLOT_LOAD,.type=fixture.cell};
        instructions[1]=instructions[2];instructions[2]=instructions[3];
        instructions[2].immediate=1;instructions[3]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={2,0}};
        actual=0;block.count=4;producer_instruction=1;call_instruction=2;
    }
    functions[4]=(XrXirFunction){.name="runtime",.name_length=7,.result=XR_XIR_I64,.blocks=&block,
        .block_count=1,.instructions=instructions,.instruction_count=block.count,.operands=&actual,.operand_count=1};
    fixture.declarations.functions=identities;fixture.module.functions=functions;fixture.module.function_count=5;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    if (status==XR_XIR_OK && oracle) CHECK(effects->root[4].requires_root==(mode!=0) &&
        !effects->root[4].unresolved && effects->root[2].unresolved);
    XirEffectProducerView producer={0};
    if (status==XR_XIR_OK) status=xir_effects_producer(context,effects,4,producer_instruction,&producer);
    InvocationRuntimeProbe read={context,mode};
    XirEffectInvocationRequest request={.caller=4,.instruction=call_instruction,.target=2,
        .producer=producer.site,.node=UINT32_MAX,.count=1,
        .read={.owner=&read,.work=invocation_runtime_work,.cell=invocation_runtime_cell,.function=NULL}};
    XirEffectInvocationSelection selection={0};
    if (status==XR_XIR_OK) {
        XrXirStatus selected=xir_effects_execution_edge(effects,&request,&selection);
        if (mode) {
            if (selected==XR_XIR_BUDGET) status=selected;
            else { CHECK(selected==XR_XIR_BAD_TYPE && !selection.owner); }
        } else status=selected;
    }
    if (status==XR_XIR_OK && oracle) {
        if (!mode) CHECK(selection.owner==effects && selection.caller==4 && selection.target==2 &&
            selection.producer==producer.site && selection.instruction==call_instruction);
        XirEffectInvocationSelection untouched=selection;
        read.cell=mode?0:1;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_TYPE &&
            !memcmp(&selection,&untouched,sizeof(selection)));
        read.cell=2;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_TYPE &&
            !memcmp(&selection,&untouched,sizeof(selection)));
        read.cell=mode;request.producer=UINT32_MAX;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&selection,&untouched,sizeof(selection)));
        request.producer=producer.site;request.instruction=producer_instruction;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&selection,&untouched,sizeof(selection)));
        request.instruction=call_instruction;request.count=0;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&selection,&untouched,sizeof(selection)));
        request.count=1;request.caller=1;request.instruction=0;
        CHECK(xir_effects_execution_edge(effects,&request,&untouched)==XR_XIR_BAD_TYPE &&
            !memcmp(&selection,&untouched,sizeof(selection)));
        request.caller=4;request.instruction=call_instruction;
    }
    xr_xir_compile_artifact_free(checked);checked=NULL;
    memset(&fixture,0,sizeof(fixture));memset(functions,0,sizeof(functions));memset(instructions,0,sizeof(instructions));
    if (status==XR_XIR_OK && oracle) {
        XirEffectInvocationSelection again={0};
        CHECK(xir_effects_execution_edge(effects,&request,&again)==(mode?XR_XIR_BAD_TYPE:XR_XIR_OK));
        if (!mode) CHECK(again.owner==selection.owner && again.node==selection.node && again.edge==selection.edge);
    }
    xr_xir_compile_effects_free(effects);return status;
}
