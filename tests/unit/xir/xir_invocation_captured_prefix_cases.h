/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_captured_prefix_cases.h - Real scalar and owned Cell prefixes
 */
typedef struct InvocationCaptureRead {
    const XrXirCompileContext *context;
    uint32_t cell;
} InvocationCaptureRead;
static bool invocation_capture_work(void *owner,uint64_t work) {
    return xir_compile_work(((InvocationCaptureRead *)owner)->context,work);
}
static uint32_t invocation_capture_cell(void *owner,uint32_t parameter) {
    InvocationCaptureRead *read=owner;CHECK(parameter==0);return read->cell;
}
static void invocation_capture_fixture(InvocationDeferredFixture *fixture,bool scalar) {
    invocation_deferred_fixture(fixture,0);
    fixture->nodes[1].parameters=NULL;fixture->nodes[1].parameter_count=0;
    fixture->nodes[1].flags=XR_XIR_CALLABLE_ROOT_NONE;
    fixture->actuals[0]=1;
    fixture->entry[2].args[1]=1;
    fixture->entry[3]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.immediate=2};
    fixture->functions[0].operand_count=1;
    fixture->apply[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    fixture->apply[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    fixture->functions[1].parameters=NULL;fixture->functions[1].parameter_count=0;
    fixture->functions[1].operands=NULL;fixture->functions[1].operand_count=0;
    if (scalar) {
        fixture->cell=XR_XIR_I64;
        fixture->entry[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
        fixture->read[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
    }
}
static XrXirStatus conditional_invocation_captured_prefix(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=2) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_capture_fixture(&fixture,mode!=0);
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;XrXirDiagnostic diagnostic={0};
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    XirEffectProducerView producer={0};
    if (status==XR_XIR_OK) status=xir_effects_producer(context,effects,0,2,&producer);
    InvocationCaptureRead read={context,0};
    XirEffectInvocationRequest request={.caller=0,.instruction=3,.target=2,.producer=producer.site,
        .node=UINT32_MAX,.count=1,.captures=1,
        .read={.owner=&read,.work=invocation_capture_work,.cell=invocation_capture_cell,.function=NULL}};
    XirEffectInvocationSelection selected={0};
    if (status==XR_XIR_OK) status=xir_effects_execution_edge(effects,&request,&selected);
    if (status==XR_XIR_OK && oracle) {
        CHECK(producer.capture_count==1 && selected.owner==effects && selected.target==2 &&
            selected.producer==producer.site && !effects->root[0].requires_root && !effects->root[0].unresolved);
        XirEffectInvocationSelection masked=selected;
        request.captures=0;
        CHECK(xir_effects_execution_edge(effects,&request,&masked)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&masked,&selected,sizeof(masked)));
        request.captures=2;
        CHECK(xir_effects_execution_edge(effects,&request,&masked)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&masked,&selected,sizeof(masked)));
        request.captures=1;request.count=2;
        CHECK(xir_effects_execution_edge(effects,&request,&masked)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&masked,&selected,sizeof(masked)));
        request.count=1;
        if (!mode) {
            CHECK(xr_xir_cell_provenance_role(effects->cells,2,0)==XR_XIR_CELL_PROOF_OWNED_CAPTURE);
            read.cell=1;
            CHECK(xir_effects_execution_edge(effects,&request,&masked)==XR_XIR_BAD_TYPE &&
                !memcmp(&masked,&selected,sizeof(masked)));
            read.cell=2;
            CHECK(xir_effects_execution_edge(effects,&request,&masked)==XR_XIR_BAD_TYPE &&
                !memcmp(&masked,&selected,sizeof(masked)));
            read.cell=0;
        }
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK && oracle) {
        XirEffectInvocationSelection again={0};XirEffectProducerView durable={0};
        CHECK(xir_effects_producer(context,effects,0,2,&durable)==XR_XIR_OK &&
            durable.capture_count==1 && durable.site==producer.site);
        CHECK(xir_effects_execution_edge(effects,&request,&again)==XR_XIR_OK && again.node==selected.node);
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,"CAPTURE_PREFIX mode=%u status=%u f=%u b=%u i=%u\n",
        mode,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    xr_xir_compile_effects_free(effects);return status;
}
