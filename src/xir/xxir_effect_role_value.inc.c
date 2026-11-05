/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_role_value.inc.c - Independent scalar views of actual role claims
 *
 * A scalar view is a claim, never an executable admission. Compile-time reads
 * frame live payloads; runtime callers retain their opaque certificate owner.
 */
static XrXirStatus effect_role_parameter_value(const EffectBodyRead *read,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,
    uint32_t function,uint32_t parameter,uint32_t *output) {
    if (!output || !proof || !effect_body_material_shape(material) ||
        function>=effect_body_count(material) || !material->functions) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&material->functions[function];
    if (parameter>=head->parameter_count || !head->parameters) return XR_XIR_BAD_STRUCTURE;
    if (!((read) && ((read)->execution ? effect_body_execution_work((read)->execution, 12+sizeof(*output)) : xir_compile_context_valid((read)->owner)))) return XR_XIR_BUDGET;
    uint32_t kind=XR_XIR_EFFECT_PARAMETER_FIXED;
    if (effect_body_variable_roles(material) &&
        xr_xir_callable_signature(material->domain.types,head->parameters[parameter])) {
        if (proof->resources!=material->resources || proof->functions!=effect_body_count(material) ||
            !proof->rows || !proof->facts || !proof->total) return XR_XIR_BAD_STRUCTURE;
        const EffectRoleFunction *row=&proof->rows[function];
        if (row->parameters!=head->parameter_count || row->instructions!=head->instruction_count ||
            row->count!=(uint64_t)head->parameter_count+head->instruction_count ||
            row->first>proof->total || row->count>proof->total-row->first || parameter>=row->count)
            return XR_XIR_BAD_STRUCTURE;
        uint32_t level=proof->facts[row->first+parameter].level;
        if (level>2) return XR_XIR_BAD_STRUCTURE;
        kind=level==2?XR_XIR_EFFECT_PARAMETER_FIXED:
            level==1?XR_XIR_EFFECT_PARAMETER_CONDITIONAL_STATIC:XR_XIR_EFFECT_PARAMETER_VARIABLE;
    }
    *output=kind;return XR_XIR_OK;
}
static XrXirStatus effect_role_parameter_read(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,
    uint32_t function,uint32_t parameter,uint32_t *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_material_frame(work,material);if (status!=XR_XIR_OK) return status;
    uint32_t functions=effect_body_count(material);
    if (functions>effect_body_entry_limit(work) || function>=functions) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,material->functions,functions,sizeof(*material->functions));
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunction *head=&material->functions[function];
    if (parameter>=head->parameter_count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,head->parameters,head->parameter_count,sizeof(*head->parameters));
    if (status!=XR_XIR_OK) return status;
    const XrXirTypes *types=material->domain.types;
    status=effect_role_range(work,types->nodes,types->count,sizeof(*types->nodes));
    if (status==XR_XIR_OK) status=effect_role_range(work,proof,1,sizeof(*proof));
    if (status!=XR_XIR_OK) return status;
    if (effect_body_variable_roles(material) && xr_xir_callable_signature(types,head->parameters[parameter])) {
        if (proof->resources!=work->resources || proof->functions!=functions || !proof->total)
            return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,proof->rows,proof->functions,sizeof(*proof->rows));
        if (status==XR_XIR_OK) status=effect_role_range(work,proof->facts,proof->total,sizeof(*proof->facts));
        if (status!=XR_XIR_OK) return status;
    }
    /* This read frames only the scalar's actual inputs. Complete closure remains
     * mandatory at admission and after the last allocator callback. */
    EffectBodyRead read=(EffectBodyRead){.owner=work};
    return effect_role_parameter_value(&read,material,proof,function,parameter,output);
}
static XrXirStatus effect_role_parameters_open(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,uint32_t function,
    uint32_t callable_parameter,EffectRoleParametersRead *output) {
    if (!output || output->work || output->material || output->proof || output->function)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_role_material_frame(work,material);if (status!=XR_XIR_OK) return status;
    uint32_t functions=effect_body_count(material);
    if (functions>effect_body_entry_limit(work) || function>=functions) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,material->functions,functions,sizeof(*material->functions));
    if (status!=XR_XIR_OK) return status;
    const EffectBodyFunction *head=&material->functions[function];
    if (callable_parameter>=head->parameter_count) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,head->parameters,head->parameter_count,sizeof(*head->parameters));
    if (status!=XR_XIR_OK) return status;
    const XrXirTypes *types=material->domain.types;
    status=effect_role_range(work,types->nodes,types->count,sizeof(*types->nodes));
    if (status==XR_XIR_OK) status=effect_role_range(work,proof,1,sizeof(*proof));
    if (status!=XR_XIR_OK) return status;

    if (!xr_xir_callable_signature(types,head->parameters[callable_parameter])) return XR_XIR_BAD_STRUCTURE;
    if (effect_body_variable_roles(material)) {
        if (proof->resources!=work->resources || proof->functions!=functions || !proof->total)
            return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,proof->rows,proof->functions,sizeof(*proof->rows));
        if (status==XR_XIR_OK) status=effect_role_range(work,proof->facts,proof->total,sizeof(*proof->facts));
        if (status!=XR_XIR_OK) return status;
        const EffectRoleFunction *row=&proof->rows[function];
        if (row->parameters!=head->parameter_count || row->instructions!=head->instruction_count ||
            row->count!=(uint64_t)head->parameter_count+head->instruction_count ||
            row->first>proof->total || row->count>proof->total-row->first) return XR_XIR_BAD_STRUCTURE;
    }

    *output=(EffectRoleParametersRead){work,material,proof,function};return XR_XIR_OK;
}
static XrXirStatus effect_role_parameters_value(const EffectRoleParametersRead *source,
    uint32_t parameter,uint32_t *output) {
    if (!source || !xir_compile_context_valid(source->work) || !source->material || !source->proof)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=(EffectBodyRead){.owner=source->work};
    return effect_role_parameter_value(&read,source->material,source->proof,source->function,parameter,output);
}

static XrXirStatus effect_role_contracts_match(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,const EffectRoleProof *proof,
    const XrXirFunctionEffectContract *contracts,uint32_t count) {
    EffectRoleCheck check={.work=work,.material=material,.proof=proof,.selected=UINT32_MAX};
    XrXirStatus status=effect_role_proof_geometry(&check);if (status!=XR_XIR_OK) return status;
    if (count!=check.functions) return XR_XIR_BAD_STRUCTURE;
    status=effect_role_range(work,contracts,count,sizeof(*contracts));if (status!=XR_XIR_OK) return status;
    EffectBodyRead read=(EffectBodyRead){.owner=work};
    for (uint32_t f=0;f<count;++f) {

        uint32_t parameters=material->functions[f].parameter_count;
        const XrXirFunctionEffectContract *contract=&contracts[f];
        if (contract->parameter_count!=parameters || !!contract->parameters!=!!parameters) return XR_XIR_BAD_STRUCTURE;
        status=effect_role_range(work,contract->parameters,parameters,sizeof(*contract->parameters));
        if (status!=XR_XIR_OK) return status;
        for (uint32_t p=0;p<parameters;++p) {
            uint32_t kind=UINT32_MAX;
            status=effect_role_parameter_value(&read,material,proof,f,p,&kind);
            if (status!=XR_XIR_OK) return status;
            if (contract->parameters[p].kind!=kind) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
