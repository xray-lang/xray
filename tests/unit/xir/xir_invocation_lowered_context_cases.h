/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_lowered_context_cases.h - Real lowered roles and advertisements
 */

static XrXirStatus invocation_lowered_context_oracle(const XrXirCompileContext *context,
    const XrXirModule *module,const XrXirEffects *effects,
    const EffectInvocationDeclaredBounds *bounds,const EffectInvocationCertificate *certificate,bool oracle) {
    const XrXirProvenance *provenance=module->provenance;
    bool variable=false,original_unknown=false,unknown_parameter=false,selected_none=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirOrigin *origin=&provenance->origins[f];
        const XrXirFunction *function=&module->functions[f];
        if (origin->function==1) {
            const XrXirFunctionEffectContract *contract=xir_effects_contract(effects,f);
            const XrXirTypeNode *physical=xr_xir_callable_signature(module->types,function->parameters[0]);
            if (!contract || !physical || contract->parameter_count!=1) return XR_XIR_BAD_STRUCTURE;
            uint32_t mask=UINT32_MAX;
            XrXirStatus status=effect_invocation_bounds_mask(context,bounds,f,0,&mask);
            if (status!=XR_XIR_OK) return status;
            uint32_t derived=UINT32_MAX;
            status=effect_invocation_certificate_node_mask(context,certificate,certificate->equations->roots[f],&derived);
            if (status!=XR_XIR_OK) return status;
            const XrXirRootFormula *formula=&certificate->solution->formulas[f];
            if (oracle) {
                CHECK(contract->parameters[0].kind==XR_XIR_EFFECT_PARAMETER_VARIABLE &&
                    contract->parameters[0].uses==XR_XIR_EFFECT_USE_INVOKE);
                CHECK(origin->effect_argument_count==function->parameter_count && origin->effect_arguments &&
                    origin->effect_arguments[0].parameter==0 && origin->effect_arguments[0].type==function->parameters[0]);
                CHECK(!formula->constant_mask && formula->term_count==(mask?1u:0u));
                if (mask) CHECK(formula->terms && formula->terms[0].kind==XR_XIR_ROOT_TERM_PARAMETER &&
                    !formula->terms[0].index);
                else CHECK(!formula->terms);
                CHECK(mask==(physical->flags&(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)));
                CHECK(derived==mask && (mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED || !mask));
            }
            variable=true;
            unknown_parameter|=mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            selected_none|=mask==0;
        }
        if (origin->function!=0) continue;
        /* COPY at this true source CALL_BIND keeps its original Fn8 bound,
         * even when the selected physical instruction is precise Fn2. */
        uint32_t mask=UINT32_MAX;
        XrXirStatus status=effect_invocation_bounds_mask(context,bounds,f,function->parameter_count+1,&mask);
        if (status!=XR_XIR_OK) return status;
        if (oracle) CHECK(mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
        original_unknown=true;
    }
    if (oracle) CHECK(variable && original_unknown && unknown_parameter && selected_none);
    return XR_XIR_OK;
}

/* The literal capture returns its original UNKNOWN declaration while the
 * real captured COPY still selects the independently precise None body. */
static XrXirStatus invocation_lowered_return_oracle(const XrXirModule *module) {
    bool found=false;
    for (uint32_t f=0;f<module->function_count;++f) {
        if (module->provenance->origins[f].function!=0) continue;
        const XrXirFunction *body=&module->functions[f];
        CHECK(body->parameter_count==0 && body->instruction_count==4);
        const XrXirInstruction *capture=&body->instructions[2];
        const XrXirInstruction *returned=&body->instructions[3];
        const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,body->result);
        CHECK(signature && signature->flags==XR_XIR_CALLABLE_ROOT_UNRESOLVED &&
            capture->op==XR_XIR_FUNCTION_REF && capture->type==body->result &&
            returned->op==XR_XIR_RETURN && returned->args[0]==2 &&
            !signature->parameter_count && signature->result==XR_XIR_I64);
        CHECK(capture->immediate>=0 && (uint64_t)capture->immediate<module->function_count &&
            capture->args[1]==1 && body->operands[capture->args[0]]==1);
        const XrXirFunction *target=&module->functions[capture->immediate];
        CHECK(target->parameter_count==1 && target->parameters);
        const XrXirTypeNode *physical=xr_xir_callable_signature(module->types,target->parameters[0]);
        CHECK(physical && physical->flags==XR_XIR_CALLABLE_ROOT_NONE &&
            body->instructions[1].op==XR_XIR_COPY && body->instructions[1].type==target->parameters[0]);
        CHECK(module->provenance->origins[capture->immediate].function==1);
        found=true;
    }
    CHECK(found);return XR_XIR_OK;
}

/* Every case traverses real construction, specialization, lowering and owned
 * full verification. A handcrafted INSTANCE or stored kind cannot satisfy it. */
static XrXirStatus conditional_invocation_lowered_context(const XrXirCompileContext *context,
    uint32_t mode,bool oracle) {
    if (mode>=2) return XR_XIR_BAD_STRUCTURE;
    RootParameterBindingFixture fixture;rp_binding_fixture(&fixture,context,mode!=0);
    XrXirArtifact *checked=NULL,*specialized=NULL,*lowered=NULL;
    XrXirEffects effects={0};EffectGraph graph={0};EffectInvocationDeclaredBounds *bounds=NULL;
    EffectInvocationCertificate *certificate=NULL;
    XrXirDiagnostic diagnostic={0};uint32_t phase=0;uint64_t selected_bodies=0;
    XrXirStatus status=xir_fixture_check(context,&fixture.module,&checked,&diagnostic);
    if (status==XR_XIR_OK) { phase=1;status=xr_xir_compile_artifact_verify(checked,&diagnostic); }
    if (status==XR_XIR_OK) { phase=2;status=xr_xir_compile_specialize(checked,&specialized,&diagnostic); }
    if (status==XR_XIR_OK) { phase=3;status=xr_xir_compile_artifact_verify(specialized,&diagnostic); }
    if (status==XR_XIR_OK && mode && oracle)
        status=invocation_lowered_return_oracle(xr_xir_compile_artifact_module(specialized));
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) { phase=4;status=xr_xir_compile_lower(specialized,&target,&lowered,&diagnostic); }
    if (status==XR_XIR_OK) { phase=5;status=xr_xir_compile_artifact_verify(lowered,&diagnostic); }
    const XrXirModule *module=lowered?xr_xir_compile_artifact_module(lowered):NULL;
    if (status==XR_XIR_OK) { phase=6;status=invocation_core_storage(context,module,&effects); }
    if (status==XR_XIR_OK) status=effect_graph_build(module,&effects,&graph,(XrXirCompileContext *)context);
    if (status==XR_XIR_OK) status=effect_parameters_derive(module,&effects,&graph,context);
    if (status==XR_XIR_OK) { phase=7;status=effect_invocation_instance_bounds(context,module,&bounds); }
    if (status==XR_XIR_OK) {
        phase=8;EffectInvocationDriverInput input={context,module,&effects,&graph,bounds,NULL};
        status=effect_invocation_derive(&input,&certificate);
    }
    if (status==XR_XIR_OK) status=invocation_lowered_context_oracle(context,module,&effects,bounds,certificate,oracle);
    if (status==XR_XIR_OK && oracle) {
        size_t attempts=rp_attempts;
        EffectInvocationDeclaredBounds *occupied=bounds;
        CHECK(effect_invocation_instance_bounds(context,module,&occupied)==XR_XIR_BAD_STRUCTURE &&
            occupied==bounds && rp_attempts==attempts);
        RootParameterMark mark=rp_mark();XrXirCompileContext foreign=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&foreign).live_bytes;
        EffectInvocationDeclaredBounds *untouched=NULL;attempts=rp_attempts;
        CHECK(effect_invocation_instance_bounds(&foreign,module,&untouched)==XR_XIR_BAD_STRUCTURE &&
            !untouched && rp_attempts==attempts);
        rp_owner_free(&foreign,baseline);rp_balanced(mark);
        uint32_t masked=UINT32_C(0xa5a55a5a);
        CHECK(effect_invocation_bounds_mask(context,bounds,bounds->count,0,&masked)==XR_XIR_BAD_STRUCTURE &&
            masked==UINT32_C(0xa5a55a5a));
    }
    if (status!=XR_XIR_OK && oracle) fprintf(stderr,
        "INVOCATION_LOWERED_CONTEXT mode=%u phase=%u status=%u f=%u b=%u i=%u\n",
        mode,phase,status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    if (status==XR_XIR_OK && oracle) {
        CHECK(module->function_count<=64 && module->function_count<=context->limits.functions);
        for (uint32_t f=0;f<module->function_count;++f)
            if (module->provenance->origins[f].function==1) selected_bodies|=UINT64_C(1)<<f;
        CHECK(selected_bodies);
    }
    effect_graph_free(&graph);invocation_core_storage_free(&effects);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(specialized);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));
    if (status==XR_XIR_OK && oracle) {
        bool found=false;
        for (uint32_t f=0;f<bounds->count;++f) {
            const EffectInvocationFunctionBounds *function=&bounds->functions[f];
            for (uint32_t r=0;r<function->count;++r) {
                uint32_t v=function->bounds[r].value;
                if (!function->bounds[r].callable ||
                    function->bounds[r].mask!=XR_XIR_CALLABLE_ROOT_UNRESOLVED) continue;
                uint32_t mask=UINT32_MAX;
                CHECK(effect_invocation_bounds_mask(context,bounds,f,v,&mask)==XR_XIR_OK &&
                    mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
                mask=UINT32_MAX;
                CHECK(effect_invocation_bounds_mask(context,certificate->declared,f,v,&mask)==XR_XIR_OK &&
                    mask==XR_XIR_CALLABLE_ROOT_UNRESOLVED);
                found=true;
            }
        }
        CHECK(found);
        bool unknown_parameter=false,selected_none=false;
        for (uint32_t f=0;f<certificate->bodies.function_count;++f) {
            CHECK(f<64);
            if (!(selected_bodies&(UINT64_C(1)<<f))) continue;
            CHECK(certificate->origins[f].function==f);
            const XrXirFunction *function=&certificate->bodies.functions[f];
            const XrXirRootFormula *formula=&certificate->solution->formulas[f];
            uint32_t original=UINT32_MAX,closed=UINT32_MAX;
            CHECK(function->parameter_count==1 && certificate->parameter_kinds[f][0]==XR_XIR_EFFECT_PARAMETER_VARIABLE);
            CHECK(effect_invocation_bounds_mask(context,certificate->declared,f,0,&original)==XR_XIR_OK);
            const XrXirTypeNode *physical=xr_xir_callable_signature(&certificate->terms.types,function->parameters[0]);
            CHECK(physical && original==(physical->flags&
                (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED)));
            CHECK(effect_invocation_certificate_node_mask(context,certificate,certificate->equations->roots[f],&closed)==XR_XIR_OK);
            CHECK(closed==original && !formula->constant_mask && formula->term_count==(original?1u:0u));
            if (original) CHECK(original==XR_XIR_CALLABLE_ROOT_UNRESOLVED && formula->terms &&
                formula->terms[0].kind==XR_XIR_ROOT_TERM_PARAMETER && !formula->terms[0].index);
            else CHECK(!formula->terms);
            unknown_parameter|=original==XR_XIR_CALLABLE_ROOT_UNRESOLVED;
            selected_none|=!original;
        }
        CHECK(unknown_parameter && selected_none);
    }
    effect_invocation_certificate_free(certificate);effect_invocation_bounds_free(bounds);return status;
}
