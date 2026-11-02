/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_function_producer.inc.c - Sealed current-frame producer selection
 *
 * KEY CONCEPT:
 *   Actual captures still pass complete admission; site identity grants no execution permission.
 */
static XrXirCallStatus instance_producer_site(XrXirInstance *instance,XrXirValueAdmission *admission,
    uint32_t caller,uint32_t instruction,uint32_t target,const XrXirProgramFunctionRef **output) {
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    if (!permissions || !permissions->effects || caller>=permissions->function_count ||
        permissions->function_count!=instance->program->entry_count) return XR_XIR_CALL_BAD_STATE;
    const XrXirProgramPermission *owner=&permissions->entries[caller];
    if (owner->reference_begin>permissions->reference_count ||
        owner->reference_count>permissions->reference_count-owner->reference_begin ||
        (owner->reference_count && !permissions->references)) return XR_XIR_CALL_BAD_STATE;
    uint64_t work=(uint64_t)owner->reference_count*5+2;
    if (!admission || admission->domain!=instance->domain || work>admission->work ||
        !xr_xir_domain_work(instance->domain,work)) return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    const XrXirProgramFunctionRef *selected=NULL;
    for (uint32_t r=0;r<owner->reference_count;++r) {
        const XrXirProgramFunctionRef *site=&permissions->references[owner->reference_begin+r];
        if (site->instruction!=instruction || (target!=UINT32_MAX && site->entry!=target)) continue;
        if (selected) return XR_XIR_CALL_BAD_STATE;
        selected=site;
    }
    if (!selected || selected->entry>=instance->program->entry_count) return XR_XIR_CALL_BAD_ARGUMENT;
    *output=selected;return XR_XIR_CALL_READY;
}

XR_FUNC XrXirCallStatus xr_xir_instance_function_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *captures,uint32_t count,XrXirValue *output) {
    if (!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if (!instance) return XR_XIR_CALL_BAD_STATE;
    if (!output || !value_unit(*output) || (count && !captures)) return XR_XIR_CALL_BAD_ARGUMENT;
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    const XrXirModule *module=instance_method_module(instance);
    if (!module || caller>=module->function_count ||
        instruction>=module->functions[caller].instruction_count ||
        module->functions[caller].instructions[instruction].op!=XR_XIR_FUNCTION_REF)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramFunctionRef *selected=NULL;
    XrXirCallStatus status=instance_producer_site(instance,xr_xir_call_admission(view),caller,instruction,UINT32_MAX,&selected);
    if (status!=XR_XIR_CALL_READY) return status;
    if (selected->captures!=count) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    const XirFunctionProducer producer={permissions->effects,caller,instruction,selected->site};
    const InstanceFunctionRequest request={selected->type,selected->entry,captures,count,&producer};
    return instance_function_create(view,&request,output);
}

static XrXirCallStatus instance_function_producer_value(XrXirInstance *instance,
    XrXirValueAdmission *admission,const XrXirValue *function,XirEffectProducerView *output) {
    if(!instance || !instance_owner_current(instance) || !admission ||
        admission->function!=admit_function_binding || admission->context!=instance ||
        admission->arena!=instance->program->arena) return XR_XIR_CALL_BAD_STATE;
    if (!function || !output) return XR_XIR_CALL_BAD_ARGUMENT;
    if (!admission || admission->domain!=instance->domain || admission->work<10 ||
        !xr_xir_domain_work(instance->domain,10)) return XR_XIR_CALL_LIMIT;
    admission->work-=10;
    if (!xr_xir_value_argument(function,instance->program->arena,(XrXirType)function->type))
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XirFunctionProducer *producer=xr_xir_function_producer(function);
    const XrXirFunctionBinding *binding=xr_xir_function_binding(function);
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    if (!producer || !permissions || producer->owner!=permissions->effects || !binding)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirModule *module=instance_method_module(instance);
    if (!module || producer->function>=module->function_count ||
        producer->instruction>=module->functions[producer->function].instruction_count)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirInstruction *op=&module->functions[producer->function].instructions[producer->instruction];
    bool method=op->op==XR_XIR_CLASS_METHOD_REF;
    if (method) {
        if (binding->capture_count!=1 || !binding->captures) return XR_XIR_CALL_BAD_ARGUMENT;
        XirClassMethodTarget target={0};
        XrXirCallStatus status=instance_method_select(instance,admission,producer->function,
            producer->instruction,&binding->captures[0],&target);
        if (status!=XR_XIR_CALL_READY) return status;
        if (binding->entry!=target.function || binding->captures[0].type!=(uint32_t)target.receiver_type)
            return XR_XIR_CALL_BAD_ARGUMENT;
    } else if (op->op!=XR_XIR_FUNCTION_REF) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramFunctionRef *site=NULL;
    XrXirCallStatus status=instance_producer_site(instance,admission,producer->function,producer->instruction,
        method?binding->entry:UINT32_MAX,&site);
    if (status!=XR_XIR_CALL_READY) return status;
    if (producer->site!=site->site || binding->entry!=site->entry ||
        binding->capture_count!=site->captures)
        return XR_XIR_CALL_BAD_ARGUMENT;
    status=admit_instance_value(admission,function,(XrXirType)function->type);
    if (status!=XR_XIR_CALL_READY) return status;
    if (function->type!=(uint32_t)site->type) {
        InstanceInvocationRead read={admission,NULL,NULL,NULL,instance};
        XrXirStatus match=xr_xir_callable_weakening_admit(instance->program->types,
            site->type,(XrXirType)function->type,&read,instance_invocation_work);
        if (match!=XR_XIR_OK) return match==XR_XIR_BUDGET?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_ARGUMENT;
    }
    uint64_t work=sizeof(*output);
    if (work>admission->work || !xr_xir_domain_work(instance->domain,work)) return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    *output=(XirEffectProducerView){site->site,producer->function,producer->instruction,
        site->entry,site->captures,site->type};return XR_XIR_CALL_READY;
}

XR_FUNC XrXirCallStatus xr_xir_instance_function_producer(XrXirCallView *view,
    const XrXirValue *function,XirEffectProducerView *output) {
    if(!instance_view_current(view)) return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    if(!instance) return XR_XIR_CALL_BAD_STATE;
    return instance_function_producer_value(instance,xr_xir_call_admission(view),function,output);
}
