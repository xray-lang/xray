/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_class_method.inc.c - Actual receiver selection in a retained Program
 *
 * KEY CONCEPT:
 *   Selection observes the complete object; only its sealed producer row may bind it.
 */
static XrXirCallStatus instance_producer_site(XrXirInstance *instance,XrXirValueAdmission *admission,
    uint32_t caller,uint32_t instruction,uint32_t target,const XrXirProgramFunctionRef **output);
static const XrXirModule *instance_method_module(const XrXirInstance *instance) {
    const XrXirProgramPermissions *permissions=instance->program->permissions;
    return permissions && permissions->effects && permissions->input && permissions->input->artifact ?
        &permissions->input->artifact->module:NULL;
}
static XrXirCallStatus instance_method_work(XrXirInstance *instance,XrXirValueAdmission *admission,
    const XrXirModule *module) {
    if (!module || !module->types || !module->types->nominals || !admission ||
        admission->domain!=instance->domain || admission->arena!=instance->program->arena)
        return XR_XIR_CALL_BAD_STATE;
    uint64_t scan=(uint64_t)module->function_count+module->types->count;
    if (scan>admission->work || !xr_xir_domain_work(instance->domain,scan)) return XR_XIR_CALL_LIMIT;
    admission->work-=scan;
    uint64_t width=1+scan+module->types->nominals->count;
    for (uint32_t f=0;f<module->function_count;++f) {
        uint64_t add=(uint64_t)module->functions[f].parameter_count+module->functions[f].consume_parameter_count;
        if (add>UINT64_MAX-width) return XR_XIR_CALL_LIMIT;
        width+=add;
    }
    for (uint32_t t=0;t<module->types->count;++t) {
        uint64_t add=module->types->nodes[t].kind==XR_XIR_TYPE_NOMINAL?
            module->types->nodes[t].nominal.field_count:0;
        if (add>UINT64_MAX-width) return XR_XIR_CALL_LIMIT;
        width+=add;
    }
    uint64_t depth=(uint64_t)module->types->nominals->count+1;
    if (width>UINT64_MAX/depth) return XR_XIR_CALL_LIMIT;
    uint64_t work=width*depth;
    if (work>admission->work || !xr_xir_domain_work(instance->domain,work)) return XR_XIR_CALL_LIMIT;
    admission->work-=work;return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_method_select(XrXirInstance *instance,XrXirValueAdmission *admission,
    uint32_t caller,uint32_t instruction,const XrXirValue *receiver,XirClassMethodTarget *output) {
    const XrXirModule *module=instance_method_module(instance);
    XrXirCallStatus status=instance_method_work(instance,admission,module);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirType actual=XR_XIR_UNIT;
    if (xir_class_actual_type(receiver,instance->program->arena,&actual)!=XR_XIR_VALUE_OK)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirStatus selected=xir_compile_class_method_site(module,caller,instruction,actual,output,NULL);
    return selected==XR_XIR_OK?XR_XIR_CALL_READY:selected==XR_XIR_BUDGET?XR_XIR_CALL_LIMIT:
        selected==XR_XIR_OUT_OF_MEMORY?XR_XIR_CALL_OOM:XR_XIR_CALL_BAD_ARGUMENT;
}
static XrXirCallStatus instance_method_binding(XrXirInstance *instance,XrXirValueAdmission *admission,
    const InstanceFunctionRequest *request,uint32_t caller,bool *output) {
    const XrXirModule *module=instance_method_module(instance);
    const XirFunctionProducer *producer=request->producer;
    if (!module || !producer || producer->owner!=instance->program->permissions->effects ||
        producer->function!=caller || caller>=module->function_count ||
        producer->instruction>=module->functions[caller].instruction_count) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirInstruction *op=&module->functions[caller].instructions[producer->instruction];
    *output=false;
    if (op->op==XR_XIR_FUNCTION_REF) return XR_XIR_CALL_READY;
    if (op->op!=XR_XIR_CLASS_METHOD_REF || request->count!=1 || !request->captures || request->type!=op->type)
        return XR_XIR_CALL_BAD_ARGUMENT;
    XirClassMethodTarget target={0};
    XrXirCallStatus status=instance_method_select(instance,admission,caller,producer->instruction,
        request->captures,&target);
    if (status!=XR_XIR_CALL_READY) return status;
    if (target.function!=request->entry || request->captures[0].type!=(uint32_t)target.receiver_type)
        return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirProgramFunctionRef *site=NULL;
    status=instance_producer_site(instance,admission,caller,producer->instruction,target.function,&site);
    if (status!=XR_XIR_CALL_READY) return status;
    if (site->site!=producer->site || site->type!=request->type || site->captures!=1)
        return XR_XIR_CALL_BAD_ARGUMENT;
    /* The site has rechecked the public anchor's imports and member access.
     * A hidden implementation does not become a separately nameable entry. */
    *output=true;return XR_XIR_CALL_READY;
}
XR_FUNC XrXirCallStatus xr_xir_instance_class_method_at(XrXirCallView *view,uint32_t instruction,
    const void *frame,XrXirValue *output) {
    if (!view || !instance_view_current(view) || !output || !value_unit(*output))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirInstance *instance=view_instance(view);
    const XrXirModule *module=instance?instance_method_module(instance):NULL;
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    if (!module || caller>=module->function_count || !instance->program->permissions->layouts)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirFunction *body=&module->functions[caller];
    const XrXirFunctionLayout *layout=&instance->program->permissions->layouts[caller];
    if (instruction>=body->instruction_count) return XR_XIR_CALL_BAD_ARGUMENT;
    const XrXirInstruction *op=&body->instructions[instruction];
    if (op->op!=XR_XIR_CLASS_METHOD_REF || op->args[0]>=(uint64_t)body->parameter_count+instruction ||
        !xir_call_frame_span(view,frame,layout->frame_bytes)) return XR_XIR_CALL_BAD_ARGUMENT;
    uint32_t offset=layout->offsets[op->args[0]];int64_t payload=0;
    if (offset>layout->frame_bytes || sizeof(payload)>layout->frame_bytes-offset)
        return XR_XIR_CALL_BAD_ARGUMENT;
    /* The two trusted compiler adapters supply their actual frame base. The
     * range check is defensive; the sealed SSA slot supplies the receiver. */
    memcpy(&payload,(const unsigned char *)frame+offset,sizeof(payload));
    XrXirValue receiver={0};
    if (!xr_xir_value_from_payload(view->arena,xr_xir_operand_type(body,op->args[0]),payload,&receiver))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirCallStatus status=xr_xir_call_execution_status(view);
    if (status!=XR_XIR_CALL_READY) return status;
    XirClassMethodTarget target={0};
    status=instance_method_select(instance,admission,caller,instruction,&receiver,&target);
    if (status!=XR_XIR_CALL_READY) return status;
    const XrXirProgramFunctionRef *site=NULL;
    status=instance_producer_site(instance,admission,caller,instruction,target.function,&site);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirValue capture={0};
    if (site->captures!=1 || site->type!=op->type ||
        !xr_xir_value_from_payload(view->arena,target.receiver_type,payload,&capture))
        return XR_XIR_CALL_BAD_ARGUMENT;
    status=xr_xir_call_execution_status(view);
    if (status!=XR_XIR_CALL_READY) return status;
    const XirFunctionProducer producer={instance->program->permissions->effects,caller,instruction,site->site};
    const InstanceFunctionRequest binding={site->type,target.function,&capture,1,&producer};
    status=instance_function_create(view,&binding,output);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_call_execution_status(view);
        if (status!=XR_XIR_CALL_READY) xr_xir_value_drop(output);
    }
    return status;
}
