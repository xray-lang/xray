/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_invocation.inc.c - Authentic actuals for sealed call equations
 *
 * KEY CONCEPT:
 *   Binding identity, domain, physical types and actual Cell storage are
 *   inspected together. A producer stamp alone never grants execution.
 */
typedef struct InstanceInvocationRead {
    XrXirValueAdmission *admission;
    const XrXirValue *arguments;
    XrXirCallView *view;
    const XrXirFunctionBinding *binding;
} InstanceInvocationRead;
static bool instance_invocation_work(void *owner,uint64_t units) {
    InstanceInvocationRead *read=owner;
    if (!read || !read->admission || units>read->admission->work ||
        !xr_xir_domain_work(read->admission->domain,units)) return false;
    read->admission->work-=units;return true;
}
static const XrXirValue *instance_invocation_parameter(const InstanceInvocationRead *read,uint32_t p) {
    uint32_t captures=read->binding?read->binding->capture_count:0;
    return p<captures?&read->binding->captures[p]:&read->arguments[p-captures];
}
static uint32_t instance_invocation_cell(void *owner,uint32_t parameter) {
    const InstanceInvocationRead *read=owner;
    const XrXirValue *actual=instance_invocation_parameter(read,parameter);
    if (!xr_xir_cell_in_domain(actual,read->admission->domain)) return 2;
    return xr_xir_cell_module_storage(actual)?1:0;
}
/* The full Program producer and binding are rechecked before exposing any
 * capture column. Physical Cell types alone cannot grant owned authority. */
static XrXirStatus instance_invocation_function_view(InstanceInvocationRead *read,uint32_t parameter,
    XirEffectProducerView *output,const XrXirFunctionBinding **captures) {
    XirEffectProducerView producer={0};
    const XrXirValue *value=instance_invocation_parameter(read,parameter);
    XrXirCallStatus status=xr_xir_instance_function_producer(read->view,value,&producer);
    if (status==XR_XIR_CALL_LIMIT) return XR_XIR_BUDGET;
    if (status==XR_XIR_CALL_OOM) return XR_XIR_OUT_OF_MEMORY;
    if (status!=XR_XIR_CALL_READY) return XR_XIR_BAD_TYPE;
    const XrXirFunctionBinding *binding=xr_xir_function_binding(value);
    const XrXirInstance *instance=view_instance(read->view);
    if (!binding || !instance || producer.target>=instance->program->entry_count ||
        binding->entry!=producer.target || binding->capture_count!=producer.capture_count)
        return XR_XIR_BAD_TYPE;
    const XrXirCallEntry *entry=&instance->program->entries[producer.target];
    if (binding->capture_count>entry->parameter_count) return XR_XIR_BAD_TYPE;
    for (uint32_t c=0;c<binding->capture_count;++c) {
        if (!instance_invocation_work(read,4)) return XR_XIR_BUDGET;
        XrXirType expected=entry->parameters[c];const XrXirValue *actual=&binding->captures[c];
        if (actual->type!=(uint32_t)expected) return XR_XIR_BAD_TYPE;
        if (xr_xir_type_is_cell(instance->program->types,expected)) {
            if (instance_cell_role((void *)instance->program,producer.target,c)!=XR_XIR_CELL_ROLE_OWNED_CAPTURE ||
                !xr_xir_cell_in_domain(actual,instance->domain) || !xr_xir_cell_unborrowed(actual) ||
                xr_xir_cell_module_storage(actual)) return XR_XIR_BAD_TYPE;
        } else if (xr_xir_type_node(instance->program->types,expected)) return XR_XIR_BAD_TYPE;
    }
    *output=producer;*captures=binding;return XR_XIR_OK;
}
static XrXirStatus instance_invocation_function(void *owner,uint32_t parameter,uint32_t *output) {
    InstanceInvocationRead *read=owner;XirEffectProducerView producer={0};
    const XrXirFunctionBinding *binding=NULL;
    XrXirStatus status=instance_invocation_function_view(read,parameter,&producer,&binding);
    if (status==XR_XIR_OK) *output=producer.site;
    return status;
}
static XrXirStatus instance_invocation_capture_cell(void *owner,uint32_t parameter,uint32_t capture,
    uint32_t site,uint32_t *output) {
    InstanceInvocationRead *read=owner;XirEffectProducerView producer={0};
    const XrXirFunctionBinding *binding=NULL;
    XrXirStatus status=instance_invocation_function_view(read,parameter,&producer,&binding);
    if (status!=XR_XIR_OK) return status;
    const XrXirInstance *instance=view_instance(read->view);
    if (site!=producer.site || capture>=binding->capture_count ||
        !xr_xir_type_is_cell(instance->program->types,(XrXirType)binding->captures[capture].type))
        return XR_XIR_BAD_TYPE;
    if (!instance_invocation_work(read,3)) return XR_XIR_BUDGET;
    /* The reader above proved real owned storage. Intrinsic access UNKNOWN is
     * independently retained by the sealed equation's closed-node check. */
    *output=0;return XR_XIR_OK;
}
static XrXirCallStatus instance_invocation_select(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *function,const XrXirValue *arguments,uint32_t count,
    XirEffectInvocationSelection *output) {
    XrXirInstance *instance=view_instance(view);
    if (!instance || !output || !function || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgram *program=instance->program;
    XirEffectProducerView producer={0};
    XrXirCallStatus status=xr_xir_instance_function_producer(view,function,&producer);
    if (status!=XR_XIR_CALL_READY) return status;
    if (producer.target>=program->entry_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirFunctionBinding *binding=xr_xir_function_binding(function);
    const XrXirTypeNode *signature=xr_xir_callable_signature(program->types,(XrXirType)function->type);
    const XrXirCallEntry *target=&program->entries[producer.target];
    if (!binding || binding->capture_count!=producer.capture_count || binding->entry!=producer.target || !signature ||
        producer.capture_count>target->parameter_count || signature->parameter_count!=count ||
        target->parameter_count-producer.capture_count!=count || signature->result!=target->result ||
        (signature->flags&XR_XIR_CALLABLE_ROOT_REQUIRED)) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    InstanceInvocationRead actual={admission,arguments,view,binding};
    if (!instance_invocation_work(&actual,12+(uint64_t)target->parameter_count*3)) return XR_XIR_CALL_LIMIT;
    for (uint32_t p=0;p<target->parameter_count;++p) {
        bool captured=p<producer.capture_count;
        XrXirType expected=target->parameters[p];
        if (!captured) {
            const XrXirCallableParameter *parameter=&signature->parameters[p-producer.capture_count];
            if (parameter->type!=expected || !xr_xir_callable_parameter_storage_valid(program->types,parameter))
                return XR_XIR_CALL_BAD_ARGUMENT;
            if (xr_xir_type_is_cell(program->types,expected) &&
                (producer.capture_count || parameter->mode!=XR_PARAM_REF))
                return XR_XIR_CALL_BAD_ARGUMENT;
        } else if (!xr_xir_type_is_cell(program->types,expected) && xr_xir_type_node(program->types,expected))
            return XR_XIR_CALL_BAD_ARGUMENT;
        const XrXirValue *value=instance_invocation_parameter(&actual,p);
        status=admit_instance_value(admission,value,expected);
        if (status!=XR_XIR_CALL_READY) return status;
        if (xr_xir_type_is_cell(program->types,expected)) {
            XrXirCellParameterRole role=captured?XR_XIR_CELL_ROLE_OWNED_CAPTURE:XR_XIR_CELL_ROLE_SCOPED_REF;
            if (instance_cell_role((void *)program,producer.target,p)!=role ||
                !xr_xir_cell_in_domain(value,instance->domain) ||
                (captured && (!xr_xir_cell_unborrowed(value) || xr_xir_cell_module_storage(value))))
                return XR_XIR_CALL_BAD_ARGUMENT;
        }
    }
    const XirEffectInvocationSelection *current=xr_xir_call_invocation(view);
    if (current && (current->owner!=program->permissions->effects ||
        current->target!=xr_xir_call_current_entry(view->activation))) return XR_XIR_CALL_BAD_STATE;
    XirEffectInvocationRequest request={.caller=xr_xir_call_current_entry(view->activation),
        .instruction=instruction,.target=producer.target,.producer=producer.site,
        .node=current?current->node:UINT32_MAX,.count=target->parameter_count,
        .captures=producer.capture_count,.contextual=current!=NULL,
        .read={.owner=&actual,.work=instance_invocation_work,.cell=instance_invocation_cell,
            .function=instance_invocation_function,.capture_cell=instance_invocation_capture_cell}};
    XrXirStatus selected=xir_effects_execution_edge(program->permissions->effects,&request,output);
    return selected==XR_XIR_OK?XR_XIR_CALL_READY:selected==XR_XIR_BUDGET?
        XR_XIR_CALL_LIMIT:selected==XR_XIR_OUT_OF_MEMORY?XR_XIR_CALL_OOM:XR_XIR_CALL_BAD_STATE;
}

static XrXirCallStatus instance_invocation_direct(XrXirCallView *view,uint32_t instruction,
    uint32_t target,const XrXirValue *arguments,uint32_t count,XirEffectInvocationSelection *output) {
    XrXirInstance *instance=view_instance(view);
    if (!instance || !output || (count && !arguments)) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgram *program=instance->program;
    if (target>=program->entry_count || count!=program->entries[target].parameter_count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    InstanceInvocationRead actual={admission,arguments,view,NULL};
    if (!instance_invocation_work(&actual,12+(uint64_t)count*3)) return XR_XIR_CALL_LIMIT;
    for (uint32_t p=0;p<count;++p) {
        XrXirType expected=program->entries[target].parameters[p];
        XrXirCallStatus status=admit_instance_value(admission,&arguments[p],expected);
        if (status!=XR_XIR_CALL_READY) return status;
        if (xr_xir_type_is_cell(program->types,expected) &&
            (instance_cell_role((void *)program,target,p)!=XR_XIR_CELL_ROLE_SCOPED_REF ||
             !xr_xir_cell_in_domain(&arguments[p],instance->domain))) return XR_XIR_CALL_BAD_ARGUMENT;
    }
    const XirEffectInvocationSelection *current=xr_xir_call_invocation(view);
    if (current && (current->owner!=program->permissions->effects ||
        current->target!=xr_xir_call_current_entry(view->activation))) return XR_XIR_CALL_BAD_STATE;
    XirEffectInvocationRequest request={.caller=xr_xir_call_current_entry(view->activation),
        .instruction=instruction,.target=target,.producer=UINT32_MAX,
        .node=current?current->node:UINT32_MAX,.count=count,.contextual=current!=NULL,
        .read={.owner=&actual,.work=instance_invocation_work,.cell=instance_invocation_cell,
            .function=instance_invocation_function,.capture_cell=instance_invocation_capture_cell}};
    XrXirStatus selected=xir_effects_execution_edge(program->permissions->effects,&request,output);
    return selected==XR_XIR_OK?XR_XIR_CALL_READY:selected==XR_XIR_BUDGET?
        XR_XIR_CALL_LIMIT:selected==XR_XIR_OUT_OF_MEMORY?XR_XIR_CALL_OOM:XR_XIR_CALL_BAD_STATE;
}
