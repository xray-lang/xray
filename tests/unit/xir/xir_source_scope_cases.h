/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_scope_cases.h - Immutable Source operations with changing actual pools
 */
#ifndef XIR_SOURCE_SCOPE_CASES_H
#define XIR_SOURCE_SCOPE_CASES_H
typedef struct SourceScopeControl {
    const XrXirCompileContext *context;
    XirEffectContextInput input;
    uint64_t limit;
    uint32_t mode,calls;
} SourceScopeControl;

static XrXirStatus source_scope_queries(void *data,XirEffectSourceScope *scope) {
    SourceScopeControl *control=data;++control->calls;
    XrXirTypes actual={0};XrXirTypeNode first={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64};
    XirEffectContextInput input=control->input;input.types=&actual;
    XirEffectContextView output={.declaration=UINT32_MAX};
    CHECK(xir_effects_source_select_scoped(scope,&input,&output)==XR_XIR_OK);
    CHECK(output.declaration==1 && output.function && output.function->result==XR_XIR_I64 &&
        !output.parameter_count && !output.argument_count && !output.requires_root && !output.unresolved);
    /* Relocation and append affect only the private actual pool. Neither
     * address equality nor the first successful query admits later inputs. */
    actual.nodes=&first;actual.count=1;
    CHECK(xir_effects_source_select_scoped(scope,&input,&output)==XR_XIR_OK);
    XrXirTypeNode relocated[2]={first,{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_BOOL}};
    actual.nodes=relocated;actual.count=2;
    CHECK(xir_effects_source_select_scoped(scope,&input,&output)==XR_XIR_OK);
    if (!control->mode) return XR_XIR_OK;
    XrXirStatus expected=control->mode==1?XR_XIR_BAD_TYPE:XR_XIR_BUDGET;
    XrXirInterfaceTable foreign={.count=1};
    if (control->mode==1) actual.interfaces=&foreign;
    else {
        uint64_t current=stats(control->context).work;CHECK(current<=control->limit);
        CHECK(xir_compile_work(control->context,control->limit-current));
    }
    XirEffectContextView rejected={.declaration=UINT32_MAX},sentinel;
    memcpy(&sentinel,&rejected,sizeof(rejected));
    CHECK(xir_effects_source_select_scoped(scope,&input,&rejected)==expected);
    CHECK(!memcmp(&rejected,&sentinel,sizeof(rejected)));
    actual.interfaces=NULL;
    XrCompileResourceStats cut=stats(control->context);
    size_t physical=live,bytes=live_bytes,tried=attempts;
    CHECK(xir_effects_source_select_scoped(scope,&input,&rejected)==expected);
    CHECK(xir_effects_source_select_scoped(scope,NULL,NULL)==expected);
    XrCompileResourceStats after=stats(control->context);
    CHECK(cut.work==after.work && cut.allocated_bytes==after.allocated_bytes &&
        cut.allocation_count==after.allocation_count && cut.live_bytes==after.live_bytes);
    CHECK(physical==live && bytes==live_bytes && tried==attempts);
    CHECK(!memcmp(&rejected,&sentinel,sizeof(rejected)));
    return XR_XIR_OK;
}

static XrXirStatus source_scope_refusal(void *data,XirEffectContextScope *scope) {
    SourceScopeControl *control=data;++control->calls;
    uint32_t output=UINT32_C(0xface1234);
    CHECK(xir_effects_reference_root_scoped(scope,control->input.source,1,0,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(output==UINT32_C(0xface1234));
    XrCompileResourceStats cut=stats(control->context);
    size_t physical=live,bytes=live_bytes,tried=attempts;
    CHECK(xir_effects_reference_root_scoped(scope,control->input.source,1,0,&output)==XR_XIR_BAD_STRUCTURE);
    CHECK(xir_effects_reference_root_scoped(scope,NULL,0,0,NULL)==XR_XIR_BAD_STRUCTURE);
    XrCompileResourceStats after=stats(control->context);
    CHECK(cut.work==after.work && cut.allocated_bytes==after.allocated_bytes &&
        cut.allocation_count==after.allocation_count && cut.live_bytes==after.live_bytes);
    CHECK(physical==live && bytes==live_bytes && tried==attempts && output==UINT32_C(0xface1234));
    return XR_XIR_OK;
}

static void source_scope_controls(const XrXirCompileContext *context,const XrXirEffects *effects,
    const XrXirModule *module,XrXirInstruction *entry) {
    XrXirOrigin origin={.function=1};
    SourceScopeControl control={.context=context,.input={module,module->types,&origin,UINT32_MAX,NULL,0}};
    CHECK(xir_effects_source_operation(context,effects,module,source_scope_queries,&control)==XR_XIR_OK &&
        control.calls==1);
    control.calls=0;control.mode=1;
    CHECK(xir_effects_source_operation(context,effects,module,source_scope_queries,&control)==XR_XIR_BAD_TYPE &&
        control.calls==1);
    control.calls=0;control.mode=0;
    CHECK(xir_effects_source_operation(context,effects,module,source_scope_queries,&control)==XR_XIR_OK &&
        control.calls==1);
    control.calls=0;++entry[0].immediate;
    CHECK(xir_effects_source_operation(context,effects,module,source_scope_queries,&control)==XR_XIR_BAD_STRUCTURE &&
        control.calls==0);
    --entry[0].immediate;
    CHECK(xir_effects_source_operation(context,effects,module,source_scope_queries,&control)==XR_XIR_OK &&
        control.calls==1);
    control.calls=0;
    CHECK(xir_effects_context_operation(context,effects,module,module->types,source_scope_refusal,&control)==
        XR_XIR_BAD_STRUCTURE && control.calls==1);
    puts("private Source operation rechecks changed actual domains, rejects damaged Source before callback, and retains sticky reference refusals PASS");
}

static void source_scope_budget_control(ProvenanceFixture *fixture) {
    CHECK(!live && !live_bytes);
    const XrCompileResourceLimits limits={8388608,4194304,4000000};
    XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();
    size_t physical=live;uint64_t bytes=live_bytes;
    XrXirInstruction ops[2]={{.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7},
        {.op=XR_XIR_RETURN,.args={0}}};
    XrXirBlock block={.count=2};
    XrXirFunction functions[2]={fixture->function,{.name="entry",.name_length=5,.result=XR_XIR_I64,
        .blocks=&block,.block_count=1,.instructions=ops,.instruction_count=2}};
    XrXirFunctionIdentity identities[2]={{0}};
    XrXirDeclarations declarations=fixture->declarations;declarations.functions=identities;
    declarations.root_module=0;declarations.entry_function=1;
    XrXirFunctionEffectContract contracts[2]={0};
    contracts[0].formula.constant_mask=XR_XIR_CALLABLE_ROOT_REQUIRED;
    XrXirProvenance evidence={.kind=XR_XIR_EVIDENCE_TEMPLATE,.contracts=contracts,.contract_count=2};
    XrXirModule module=fixture->module;module.functions=functions;module.function_count=2;
    module.declarations=&declarations;module.linkage_kind=XR_XIR_PROGRAM;module.provenance=&evidence;
    XrXirConstruction *construction=NULL;XrXirEffects *effects=NULL;
    CHECK(xir_fixture_construction(&context,module.types,&construction)==XR_XIR_OK);
    CHECK(xr_xir_compile_verify_owned_effects_v2(&context,&module,construction,&effects,NULL)==XR_XIR_OK);
    xr_xir_compile_construction_free(construction);
    XrXirOrigin origin={.function=1};
    SourceScopeControl control={.context=&context,.input={&module,module.types,&origin,UINT32_MAX,NULL,0},
        .limit=limits.work,.mode=2};
    CHECK(xir_effects_source_operation(&context,effects,&module,source_scope_queries,&control)==XR_XIR_BUDGET &&
        control.calls==1 && stats(&context).work==limits.work);
    xr_xir_compile_effects_free(effects);owner_free(&context,physical,bytes);
    puts("private Source operation ignores no budget failure and releases all physical blocks PASS");
}

#endif // XIR_SOURCE_SCOPE_CASES_H
