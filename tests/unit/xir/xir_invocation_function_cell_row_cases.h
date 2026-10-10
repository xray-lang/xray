/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_function_cell_row_cases.h - Complete returned actual environment rows
 */
typedef struct InvocationFunctionCellRead {
    const XrXirCompileContext *context;
    const XrXirEffects *effects;
    uint32_t instruction,origin;
    bool unknown,wrong_site;
} InvocationFunctionCellRead;
static bool invocation_function_cell_work(void *owner,uint64_t units) {
    return xir_compile_work(((InvocationFunctionCellRead *)owner)->context,units);
}
static uint32_t invocation_function_cell_actual(void *owner,uint32_t parameter) {
    InvocationFunctionCellRead *read=owner;CHECK(parameter==0);return read->origin;
}
static XrXirStatus invocation_function_cell_site(void *owner,uint32_t parameter,uint32_t *output) {
    InvocationFunctionCellRead *read=owner;CHECK(parameter==0);
    if (read->unknown) return XR_XIR_BAD_TYPE;
    XirEffectProducerView producer={0};
    XrXirStatus status=xir_effects_producer(read->context,read->effects,1,read->instruction,&producer);
    if (status==XR_XIR_OK) {
        CHECK(producer.capture_count==1);
        *output=read->wrong_site?UINT32_MAX:producer.site;
    }
    return status;
}
static XrXirStatus invocation_function_cell_capture(void *owner,uint32_t parameter,uint32_t capture,
    uint32_t site,uint32_t *output) {
    InvocationFunctionCellRead *read=owner;CHECK(parameter==0 && capture==0);
    XirEffectProducerView producer={0};
    XrXirStatus status=xir_effects_producer(read->context,read->effects,1,read->instruction,&producer);
    if (status!=XR_XIR_OK) return status;
    if (site!=producer.site) return XR_XIR_BAD_TYPE;
    *output=read->origin;return XR_XIR_OK;
}

/* This is a real Checked module, with the factory's returned Function passed
 * as an actual to a distinct body. The Cell prefix keeps its OWNED role. */
static XrXirStatus conditional_invocation_function_cell_row(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=2) return XR_XIR_BAD_STRUCTURE;
    InvocationReturnedFixture fixture;invocation_returned_fixture(&fixture,mode?5:6);
    XrXirType parameter=(XrXirType)256;uint32_t actual=0;
    fixture.entry[1]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.immediate=3,.args={0,1}};
    fixture.functions[0].operands=&actual;fixture.functions[0].operand_count=1;
    fixture.relay[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    fixture.relay[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    fixture.functions[3].parameters=&parameter;fixture.functions[3].parameter_count=1;
    fixture.functions[3].result=XR_XIR_I64;
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    InvocationFunctionCellRead read={context,effects,mode?1u:2u,0,false,false};
    XirEffectInvocationRequest direct={.caller=0,.instruction=1,.target=3,.producer=UINT32_MAX,
        .node=UINT32_MAX,.count=1,
        .read={.owner=&read,.work=invocation_function_cell_work,.cell=invocation_function_cell_actual,
            .function=invocation_function_cell_site,.capture_cell=invocation_function_cell_capture}};
    XirEffectInvocationSelection selected={0},child={0};XirEffectProducerView producer={0};
    if (status==XR_XIR_OK) status=xir_effects_execution_edge(effects,&direct,&selected);
    if (status==XR_XIR_OK) status=xir_effects_producer(context,effects,1,read.instruction,&producer);
    XirEffectInvocationRequest indirect={.caller=3,.instruction=0,.target=2,.producer=producer.site,
        .node=selected.node,.count=1,.captures=1,.contextual=true,.read=direct.read};
    if (status==XR_XIR_OK) status=xir_effects_execution_edge(effects,&indirect,&child);
    if (status==XR_XIR_OK && oracle) {
        const EffectInvocationCertificate *certificate=effects->invocations;
        const EffectInvocationOwner *owner=certificate->equations;
        CHECK(producer.capture_count==1 && selected.owner==effects && selected.target==3 &&
            child.parent==selected.node && child.target==2 && !effects->root[0].requires_root &&
            !effects->root[0].unresolved && !certificate->declared->functions[3].bounds[0].mask);
        if (!mode) CHECK(xr_xir_cell_provenance_role(effects->cells,2,0)==XR_XIR_CELL_PROOF_OWNED_CAPTURE);
        CHECK(owner->nodes[selected.node].parameter_count==1 && owner->sites[producer.site].captures==1);
        XirEffectInvocationSelection masked=selected;
        read.unknown=true;
        CHECK(xir_effects_execution_edge(effects,&direct,&masked)==XR_XIR_BAD_TYPE &&
            !memcmp(&masked,&selected,sizeof(masked)));read.unknown=false;
        read.wrong_site=true;
        CHECK(xir_effects_execution_edge(effects,&direct,&masked)==XR_XIR_BAD_TYPE &&
            !memcmp(&masked,&selected,sizeof(masked)));read.wrong_site=false;
        if (!mode) {
            direct.read.capture_cell=NULL;
            CHECK(xir_effects_execution_edge(effects,&direct,&masked)==XR_XIR_BAD_TYPE &&
                !memcmp(&masked,&selected,sizeof(masked)));direct.read.capture_cell=invocation_function_cell_capture;
            for (uint32_t origin=1;origin<3;++origin) {
                read.origin=origin;
                CHECK(xir_effects_execution_edge(effects,&direct,&masked)==XR_XIR_BAD_TYPE &&
                    !memcmp(&masked,&selected,sizeof(masked)));
            }
            read.origin=0;
        }
        indirect.contextual=false;indirect.node=UINT32_MAX;masked=child;
        CHECK(xir_effects_execution_edge(effects,&indirect,&masked)==XR_XIR_BAD_TYPE &&
            !memcmp(&masked,&child,sizeof(masked)));indirect.contextual=true;indirect.node=selected.node;
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));parameter=XR_XIR_UNIT;actual=UINT32_MAX;
    if (status==XR_XIR_OK && oracle) {
        XirEffectInvocationSelection durable={0};
        CHECK(xir_effects_execution_edge(effects,&direct,&durable)==XR_XIR_OK && durable.node==selected.node);
        CHECK(xir_effects_execution_edge(effects,&indirect,&durable)==XR_XIR_OK && durable.node==child.node);
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,"FUNCTION_CELL_ROW mode=%u status=%u f=%u b=%u i=%u\n",
        mode,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}
