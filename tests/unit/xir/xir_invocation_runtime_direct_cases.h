/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_runtime_direct_cases.h - Actual direct to indirect owned context
 */
typedef struct InvocationDirectRead {
    const XrXirCompileContext *context;
    const XrXirEffects *effects;
    uint32_t instruction;
    bool unknown;
} InvocationDirectRead;
static bool invocation_direct_work(void *owner,uint64_t work) {
    return xir_compile_work(((InvocationDirectRead *)owner)->context,work);
}
static uint32_t invocation_direct_cell(void *owner,uint32_t parameter) {
    (void)owner;CHECK(parameter<=1);return 0;
}
static XrXirStatus invocation_direct_function(void *owner,uint32_t parameter,uint32_t *output) {
    InvocationDirectRead *read=owner;CHECK(parameter==0);
    if (read->unknown) return XR_XIR_BAD_TYPE;
    XirEffectProducerView producer={0};
    XrXirStatus status=xir_effects_producer(read->context,read->effects,0,read->instruction,&producer);
    if (status==XR_XIR_OK) { CHECK(!producer.capture_count);*output=producer.site; }
    return status;
}
static XrXirStatus conditional_invocation_runtime_direct(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=3) return XR_XIR_BAD_STRUCTURE;
    InvocationDeferredFixture fixture;invocation_deferred_fixture(&fixture,0);
    XrXirTypeNode nodes[3];memcpy(nodes,fixture.nodes,sizeof(fixture.nodes));
    XrXirInstruction apply[3];XrXirBlock block={.count=3};
    if (mode) {
        fixture.ref=(XrXirCallableParameter){XR_XIR_I64,XR_PARAM_READ};
        nodes[1].flags=mode==2?XR_XIR_CALLABLE_ROOT_UNRESOLVED:XR_XIR_CALLABLE_ROOT_NONE;
        nodes[2]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)257};
        fixture.types.nodes=nodes;fixture.types.count=3;
        fixture.parameters[1]=XR_XIR_I64;fixture.cell=XR_XIR_I64;
        fixture.entry[1]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
        fixture.read[0]=(XrXirInstruction){.op=XR_XIR_COPY,.type=XR_XIR_I64};
        apply[0]=(XrXirInstruction){.op=XR_XIR_NULLABLE_SOME,.type=(XrXirType)258};
        apply[1]=fixture.apply[0];apply[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={3,0}};
        fixture.functions[1].instructions=apply;fixture.functions[1].instruction_count=3;
        fixture.functions[1].blocks=&block;
    }
    XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_effects_analyze(checked,&effects);
    InvocationDirectRead read={context,effects,2,false};
    XirEffectInvocationRequest direct={.caller=0,.instruction=3,.target=1,.producer=UINT32_MAX,
        .node=UINT32_MAX,.count=2,
        .read={.owner=&read,.work=invocation_direct_work,.cell=invocation_direct_cell,.function=invocation_direct_function}};
    XirEffectInvocationSelection selected={0},child={0};XirEffectProducerView producer={0};
    if (status==XR_XIR_OK) {
        XrXirStatus chosen=xir_effects_execution_edge(effects,&direct,&selected);
        if (mode==2) {
            if (chosen==XR_XIR_BUDGET) status=chosen;
            else CHECK(chosen==XR_XIR_BAD_TYPE && !selected.owner &&
                effects->root[0].unresolved && effects->root[1].unresolved);
        } else status=chosen;
    }
    if (status==XR_XIR_OK) status=xir_effects_producer(context,effects,0,2,&producer);
    XirEffectInvocationRequest indirect={.caller=1,.instruction=mode?1:0,.target=2,
        .producer=producer.site,.node=selected.node,.count=1,.contextual=true,
        .read={.owner=&read,.work=invocation_direct_work,.cell=invocation_direct_cell,.function=invocation_direct_function}};
    if (status==XR_XIR_OK && mode!=2) status=xir_effects_execution_edge(effects,&indirect,&child);
    if (status==XR_XIR_OK && oracle && mode!=2) {
        CHECK(!effects->root[0].requires_root && !effects->root[0].unresolved &&
            effects->root[1].unresolved==(mode==0));
        CHECK(selected.owner==effects && selected.target==1 && child.owner==effects && child.caller==1 &&
            child.target==2 && child.parent==selected.node);
        if (mode==1) {
            const EffectInvocationCertificate *certificate=effects->invocations;
            const EffectInvocationNode *node=&certificate->equations->nodes[selected.node];
            uint32_t opaque=certificate->equations->site_count+
                certificate->bodies.functions[node->root].parameter_count;
            CHECK(certificate->parameter_kinds[1][0]==XR_XIR_EFFECT_PARAMETER_FIXED &&
                !certificate->declared->functions[1].bounds[0].mask &&
                (node->input[producer.site/64]&(UINT64_C(1)<<(producer.site%64))) &&
                (node->input[opaque/64]&(UINT64_C(1)<<(opaque%64))));
        }
        XirEffectInvocationSelection masked=child;
        indirect.contextual=false;indirect.node=UINT32_MAX;
        CHECK(xir_effects_execution_edge(effects,&indirect,&masked)==XR_XIR_BAD_TYPE &&
            !memcmp(&masked,&child,sizeof(masked)));
        indirect.contextual=true;indirect.node=selected.node;
        read.unknown=true;masked=selected;
        CHECK(xir_effects_execution_edge(effects,&direct,&masked)==XR_XIR_BAD_TYPE &&
            !memcmp(&masked,&selected,sizeof(masked)));read.unknown=false;
        direct.target=2;
        CHECK(xir_effects_execution_edge(effects,&direct,&masked)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&masked,&selected,sizeof(masked)));direct.target=1;
        indirect.node=child.node;masked=child;
        CHECK(xir_effects_execution_edge(effects,&indirect,&masked)==XR_XIR_BAD_STRUCTURE &&
            !memcmp(&masked,&child,sizeof(masked)));indirect.node=selected.node;
    }
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));
    memset(nodes,0,sizeof(nodes));memset(apply,0,sizeof(apply));
    if (status==XR_XIR_OK && oracle && mode!=2) {
        XirEffectInvocationSelection again={0};
        CHECK(xir_effects_execution_edge(effects,&direct,&again)==XR_XIR_OK && again.node==selected.node);
        CHECK(xir_effects_execution_edge(effects,&indirect,&again)==XR_XIR_OK && again.node==child.node);
    }
    xr_xir_compile_effects_free(effects);return status;
}
