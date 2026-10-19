/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_instance_bounds.inc.c - Real instance advertisements
 *
 * KEY CONCEPT:
 *   Complete provenance checking precedes this receiving owner. The source
 *   keeps original instruction advertisements; the authentic selected vector
 *   keeps physical formal bounds. Neither is reconstructed from final SSA flags.
 */

static XrXirStatus effect_invocation_instance_bound(const XrXirCompileContext *work,
    const XirEffectCallableBound *request,EffectInvocationValueBound *output) {
    if (!request || !output || !!request->arguments!=!!request->argument_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3+sizeof(*output))) return XR_XIR_BUDGET;
    XirEffectCallableBound bound=*request;
    uint32_t id=(uint32_t)bound.declared;
    if (id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t parameter=id-XR_XIR_TYPE_PARAMETER_BASE;
        if (parameter>=bound.argument_count) return XR_XIR_BAD_TYPE;
        bound.declared=bound.arguments[parameter];bound.source=bound.destination;
        bound.arguments=NULL;bound.argument_count=0;
    }
    const XrXirTypeNode *declared=xr_xir_callable_signature(bound.source,bound.declared);
    const XrXirTypeNode *actual=xr_xir_callable_signature(bound.destination,bound.actual);
    if (!!declared!=!!actual) return XR_XIR_BAD_TYPE;
    if (!declared) { *output=(EffectInvocationValueBound){0};return XR_XIR_OK; }
    XrXirStatus status=xir_effect_callable_bound_matches(work,&bound);
    if (status!=XR_XIR_OK) return status;
    *output=(EffectInvocationValueBound){.mask=declared->flags&
        (XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED),.callable=true};
    return XR_XIR_OK;
}

static XrXirStatus effect_invocation_instance_function(const XrXirCompileContext *work,
    const XrXirModule *module,uint32_t f,EffectInvocationDeclaredBounds *bounds) {
    const XrXirProvenance *provenance=module->provenance;
    const XrXirModule *source=&provenance->source->module;
    const XrXirOrigin *origin=&provenance->origins[f];
    const XrXirFunction *body=&module->functions[f],*original=&source->functions[origin->function];
    if (body->parameter_count!=original->parameter_count || body->instruction_count!=original->instruction_count ||
        (body->parameter_count && !body->parameters) || (body->instruction_count && !body->instructions) ||
        (original->instruction_count && !original->instructions)) return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionEffectContract *contract=NULL;
    if (source->provenance) {
        const XrXirProvenance *template=source->provenance;
        if (template->kind!=XR_XIR_EVIDENCE_TEMPLATE || template->contract_count!=source->function_count ||
            !template->contracts) return XR_XIR_BAD_STRUCTURE;
        contract=&template->contracts[origin->function];
        if (contract->value_count && !contract->values) return XR_XIR_BAD_STRUCTURE;
    }
    uint64_t values=(uint64_t)body->parameter_count+body->instruction_count;
    if (values>UINT32_MAX || values>SIZE_MAX/sizeof(EffectInvocationValueBound)) return XR_XIR_BUDGET;
    XrXirStatus status=effect_invocation_bounds_capacity(work,bounds,f+1);
    if (status!=XR_XIR_OK) return status;
    uint32_t count=0;
    for (uint32_t v=0;v<(uint32_t)values;++v) {
        if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(module->types,xr_xir_operand_type(body,v))) ++count;
    }
    EffectInvocationValueBound *records=count ?
        xir_compile_alloc(work,(size_t)count*sizeof(*records),&status) : NULL;
    if (count && !records) return status;
    uint32_t identity=0,at=0;
    for (uint32_t v=0;v<(uint32_t)values && status==XR_XIR_OK;++v) {
        if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
        XrXirType actual=xr_xir_operand_type(body,v);
        XirEffectCallableBound request={source->types,module->types,origin->arguments,
            origin->argument_count,XR_XIR_UNIT,actual};
        if (v<body->parameter_count) {
            /* This is the selected physical vector, not a refined instruction. */
            if (source->provenance && (origin->effect_argument_count!=body->parameter_count ||
                !origin->effect_arguments || origin->effect_arguments[v].parameter!=v ||
                origin->effect_arguments[v].type!=actual)) { status=XR_XIR_BAD_STRUCTURE;break; }
            request.source=module->types;request.arguments=NULL;request.argument_count=0;
            request.declared=actual;
        } else {
            uint32_t instruction=v-body->parameter_count;
            request.declared=original->instructions[instruction].type;
            if (contract && identity<contract->value_count) {
                if (!xir_compile_work(work,2)) { status=XR_XIR_BUDGET;break; }
                const XrXirRootValueIdentity *value=&contract->values[identity];
                if (value->instruction<instruction) { status=XR_XIR_BAD_STRUCTURE;break; }
                if (value->instruction==instruction) {
                    request.declared=value->declared_type;++identity;
                }
            }
        }
        EffectInvocationValueBound record={0};
        status=effect_invocation_instance_bound(work,&request,&record);
        if (status==XR_XIR_OK && record.callable) {
            if (at>=count) { status=XR_XIR_BAD_STRUCTURE;break; }
            if (!xir_compile_work(work,sizeof(record))) { status=XR_XIR_BUDGET;break; }
            record.value=v;records[at++]=record;
        }
    }
    if (status==XR_XIR_OK && ((contract && identity!=contract->value_count) || at!=count)) status=XR_XIR_BAD_STRUCTURE;
    if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(*bounds->functions)+1)) status=XR_XIR_BUDGET;
    if (status!=XR_XIR_OK) { xr_compile_resources_free(records);return status; }
    bounds->functions[f]=(EffectInvocationFunctionBounds){(uint32_t)values,count,records};
    ++bounds->count;return XR_XIR_OK;
}

/* Private receiving construction after full source/body/origin/owner replay.
 * This copies bounds only. It does not seal equations, relax a checker or grant
 * Program/runtime permission. A failed or occupied request publishes nothing. */
static inline XrXirStatus effect_invocation_instance_bounds(const XrXirCompileContext *work,
    const XrXirModule *module,EffectInvocationDeclaredBounds **output) {
    if (!xir_compile_context_valid(work) || !module || !module->functions || !module->function_count ||
        !output || *output || !module->provenance || module->provenance->kind!=XR_XIR_EVIDENCE_INSTANCE)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirProvenance *provenance=module->provenance;
    if (!provenance->source || provenance->source->context.resources!=work->resources)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_contract_instance(work,module,provenance);
    if (status!=XR_XIR_OK) return status;
    EffectInvocationDeclaredBounds *bounds=NULL;
    status=effect_invocation_bounds_new(work,&bounds);
    for (uint32_t f=0;status==XR_XIR_OK && f<module->function_count;++f)
        status=effect_invocation_instance_function(work,module,f,bounds);
    if (status!=XR_XIR_OK) { effect_invocation_bounds_free(bounds);return status; }
    *output=bounds;return XR_XIR_OK;
}
