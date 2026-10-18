/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_generator.inc.c - Creation from sealed canonical instruction sites
 *
 * KEY CONCEPT:
 *   Only the owning executor holds code authority for a live producer.
 */
XR_FUNC XrXirCallStatus xr_xir_instance_generator_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *arguments,uint32_t count,XrXirValue *output) {
    if(!view || !instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if(!instance) return XR_XIR_CALL_BAD_STATE;
    if(!output || !value_unit(*output) || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    if(!permissions || permissions->function_count!=instance->program->entry_count ||
        caller>=permissions->function_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirProgramPermission *owner=&permissions->entries[caller];
    if(owner->generator_begin>permissions->generator_count ||
        owner->generator_count>permissions->generator_count-owner->generator_begin ||
        (owner->generator_count && !permissions->generators)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    uint64_t work=(uint64_t)owner->generator_count*4+2;
    if(!admission || work>admission->work || !xr_xir_domain_work(instance->domain,work)) return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    const XrXirProgramGeneratorSite *selected=NULL;
    for(uint32_t i=0;i<owner->generator_count;++i) {
        const XrXirProgramGeneratorSite *site=&permissions->generators[owner->generator_begin+i];
        if(site->instruction!=instruction) continue;
        if(selected) return XR_XIR_CALL_BAD_STATE;
        selected=site;
    }
    if(!selected || selected->entry>=instance->program->entry_count || selected->arguments!=count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirCallRequest request={selected->entry,arguments,count,instance_cell_role,instance->program};
    return xr_xir_task_executor_generator(instance->executor,view,selected->type,&request,output,instruction);
}

XR_FUNC XrXirCallStatus xr_xir_instance_generator_context(XrXirCallView *view,uint32_t instruction,
    const XrXirCallRequest *request,XirEffectInvocationSelection *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance || !request || !output || (request->count && !request->arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgram *program=instance->program;
    if (request->entry>=program->entry_count || !program->permissions ||
        request->cell_role!=instance_cell_role || request->cell_context!=program ||
        !(program->entries[request->entry].flags&XR_XIR_ENTRY_GENERATOR) ||
        request->count!=program->entries[request->entry].parameter_count) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    InstanceInvocationRead actual={admission,request->arguments,view,NULL};
    if (!instance_invocation_work(&actual,12+(uint64_t)request->count*4)) return XR_XIR_CALL_LIMIT;
    for (uint32_t p=0;p<request->count;++p) {
        XrXirType type=program->entries[request->entry].parameters[p];
        const XrXirValue *value=&request->arguments[p];
        XrXirCallStatus status=admit_instance_value(admission,value,type);
        if (status!=XR_XIR_CALL_READY) return status;
        if (xr_xir_type_is_cell(program->types,type) &&
            (instance_cell_role((void *)program,request->entry,p)!=XR_XIR_CELL_ROLE_OWNED_CAPTURE ||
             !xr_xir_cell_in_domain(value,instance->domain) || !xr_xir_cell_unborrowed(value) ||
             xr_xir_cell_module_storage(value))) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    const XirEffectInvocationSelection *current=xr_xir_call_invocation(view);
    if (current && (current->owner!=program->permissions->effects || current->target!=caller))
        return XR_XIR_CALL_BAD_STATE;
    XirEffectInvocationRequest selected={.caller=caller,.instruction=instruction,.target=request->entry,
        .producer=UINT32_MAX,.node=current?current->node:UINT32_MAX,.count=request->count,
        .contextual=current!=NULL,.read={.owner=&actual,.work=instance_invocation_work,.cell=instance_invocation_cell,
            .class_value=instance_invocation_class,.function=instance_invocation_function,.capture_cell=instance_invocation_capture_cell}};
    XrXirStatus status=xir_effects_execution_edge(program->permissions->effects,&selected,output);
    return status==XR_XIR_OK?XR_XIR_CALL_READY:status==XR_XIR_BUDGET?XR_XIR_CALL_LIMIT:
        status==XR_XIR_OUT_OF_MEMORY?XR_XIR_CALL_OOM:XR_XIR_CALL_BAD_STATE;
}
