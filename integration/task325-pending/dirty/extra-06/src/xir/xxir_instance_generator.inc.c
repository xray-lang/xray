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
    const XirGeneratorSiteOrigin origin={instruction,selected->site};
    return xr_xir_task_executor_generator(instance->executor,view,selected->type,&request,output,&origin);
}

/* A genuine executing view may name one frontier. The parked wait reader and
 * complete Effects query remain responsible for authenticating the action. */
XR_FUNC XrXirCallStatus xr_xir_instance_iterator_at(XrXirCallView *view,uint32_t instruction) {
    if(!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    if(!view_instance(view)) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_call_iterator_stage(view,instruction);
}

/* The exact linked waiter owns these borrowed inputs throughout this read.
 * No executing view, creation receipt or nominal body result is substituted. */
XR_FUNC XrXirCallStatus xr_xir_instance_iterator_select(XrXirCall *call,
    const XrXirTaskWaitToken *token,XirEffectInvocationSelection *output) {
    if(!output) return XR_XIR_CALL_BAD_ARGUMENT;
    XirIteratorWaitContext wait={0};XrXirCallStatus checked=xr_xir_call_iterator_context(call,token,&wait);
    if(checked!=XR_XIR_CALL_READY) return checked;
    XrXirValueAdmission *admission=wait.admission;
    if(!admission || admission->function!=admit_function_binding || !wait.instance ||
        !instance_owner_current(wait.instance)) return XR_XIR_CALL_BAD_STATE;
    XrXirInstance *root=wait.instance,*active=instance_current(root),*instance=admission->context;
    if(root->owner || !active || admission->domain!=root->domain) return XR_XIR_CALL_BAD_STATE;
    bool known=instance==active || instance==root;
    for(XrXirInstance *state=known?NULL:root->generation_next;state;state=state->generation_next) {
        if(!admission->work || !xr_xir_domain_work(admission->domain,1)) return XR_XIR_CALL_LIMIT;
        --admission->work;
        if(state==instance) {known=true;break;}
    }
    if(!known || !instance || !instance->program || !instance_owner_current(instance) ||
        instance_root(instance)!=root ||
        admission->domain!=instance->domain || admission->arena!=instance->program->arena ||
        !instance->driving || instance_observing(instance) || instance->stopping ||
        !active->driving || instance_observing(active) || active->stopping ||
        token->binding.owner!=active->executor) return XR_XIR_CALL_BAD_STATE;
    checked=xr_xir_task_executor_pending_member(active->executor,call,token,admission);
    if(checked!=XR_XIR_CALL_READY) return checked;
    const XrXirProgram *program=instance->program;
    if(!program->permissions || !program->permissions->effects || wait.caller>=program->entry_count ||
        !wait.iterator || (wait.invocation && (wait.invocation->owner!=program->permissions->effects ||
        wait.invocation->target!=wait.caller))) return XR_XIR_CALL_BAD_STATE;
    InstanceInvocationRead actual={admission,NULL,NULL,NULL,instance};
    XirEffectRuntimeRead read={.owner=&actual,.work=instance_invocation_work,.cell=instance_invocation_cell,
        .class_value=instance_invocation_class,.value=instance_invocation_value,.allocate=instance_invocation_allocate,
        .deallocate=instance_invocation_deallocate};
    XirEffectIteratorInstruction instruction={0};
    XrXirStatus status=xir_effects_iterator_instruction(program->permissions->effects,wait.caller,wait.instruction,&read,&instruction);
    uint32_t opcode=token->request.kind==XR_XIR_WAIT_ITERATOR_NEXT?XR_XIR_ITERATOR_NEXT:
        token->request.kind==XR_XIR_WAIT_ITERATOR_HAS_NEXT?XR_XIR_ITERATOR_HAS_NEXT:
        token->request.kind==XR_XIR_WAIT_ITERATOR_NTH?XR_XIR_ITERATOR_NTH:UINT32_MAX;
    if(status==XR_XIR_OK && (instruction.opcode!=opcode || instruction.receiver!=(XrXirType)wait.iterator->type))
        status=XR_XIR_BAD_TYPE;
    XirEffectProducerView producer={0};const XrXirValue *values=NULL;uint32_t count=0;
    if(status==XR_XIR_OK) status=instance_invocation_capture_values(&actual,wait.iterator,&producer,&values,&count);
    if(status!=XR_XIR_OK) return status==XR_XIR_BUDGET?XR_XIR_CALL_LIMIT:
        status==XR_XIR_OUT_OF_MEMORY?XR_XIR_CALL_OOM:XR_XIR_CALL_BAD_STATE;
    actual.arguments=values;
    for(uint32_t p=0;p<count;++p) {
        XrXirType type=program->entries[producer.target].parameters[p];
        checked=admit_instance_value(admission,&values[p],type);if(checked!=XR_XIR_CALL_READY) return checked;
        if(xr_xir_type_is_cell(program->types,type) &&
            (instance_cell_role((void *)program,producer.target,p)!=XR_XIR_CELL_ROLE_OWNED_CAPTURE ||
             !xr_xir_cell_in_domain(&values[p],instance->domain) || !xr_xir_cell_unborrowed(&values[p]) ||
             xr_xir_cell_module_storage(&values[p]))) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    XirEffectInvocationRequest request={.caller=wait.caller,.instruction=wait.instruction,.target=producer.target,
        .producer=producer.site,.node=wait.invocation?wait.invocation->node:UINT32_MAX,.count=count,.captures=count,
        .contextual=wait.invocation!=NULL,.read=read};XirEffectInvocationSelection selected={0};
    status=xir_effects_execution_edge(program->permissions->effects,&request,&selected);
    if(status!=XR_XIR_OK) return status==XR_XIR_BUDGET?XR_XIR_CALL_LIMIT:
        status==XR_XIR_OUT_OF_MEMORY?XR_XIR_CALL_OOM:XR_XIR_CALL_BAD_STATE;
    XirIteratorWaitContext current={0};checked=xr_xir_call_iterator_context(call,token,&current);
    if(checked!=XR_XIR_CALL_READY) return checked;
    if(current.instance!=wait.instance || current.admission!=admission || current.iterator!=wait.iterator ||
        current.invocation!=wait.invocation || current.caller!=wait.caller || current.instruction!=wait.instruction)
        return XR_XIR_CALL_BAD_STATE;
    checked=xr_xir_task_executor_pending_member(active->executor,call,token,admission);
    if(checked!=XR_XIR_CALL_READY) return checked;
    *output=selected;return XR_XIR_CALL_READY;
}
