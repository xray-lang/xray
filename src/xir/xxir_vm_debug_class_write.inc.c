/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_vm_debug_class_write.inc.c - Prepare one identity field under a live scope
 */
typedef struct DebugClassWrite {
    XrXirValue root,candidate;
    XrXirCallClassAccess access;
    XrXirValuePathStep field;
} DebugClassWrite;
static void debug_class_write_drop(DebugClassWrite *write) {
    if (write->access.value) xr_xir_call_class_access_finish(&write->access);
    xr_xir_value_drop(&write->candidate);xr_xir_value_drop(&write->root);
}
static XrXirValueStatus debug_write_member(XrXirVmDebug *debug,uint32_t function,
    const XrXirValuePathStep *step,XrXirNominalAccess access) {
    if (!xr_xir_domain_work(debug->domain,1)) return XR_XIR_VALUE_LIMIT;
    if (step->kind!=XR_XIR_PATH_FIELD) return XR_XIR_VALUE_OK;
    const XrXirModule *module=xr_xir_compile_artifact_module(debug->artifact);
    const XrXirTypeNode *node=xr_xir_type_node(module->types,step->container);
    if (!node || node->kind!=XR_XIR_TYPE_NOMINAL || step->selector<0 || (uint64_t)step->selector>UINT32_MAX)
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirStatus status=xr_xir_compile_nominal_access(&debug->artifact->context,module,function,
        node->nominal.declaration,(uint32_t)step->selector,access);
    return status==XR_XIR_OK ? XR_XIR_VALUE_OK : status==XR_XIR_BUDGET ? XR_XIR_VALUE_LIMIT : XR_XIR_VALUE_BAD_ARGUMENT;
}
static XrXirValueStatus debug_class_write_read(XrXirCallView *view,const XrXirValue *value,
    const XrXirValuePathStep *step,XrXirValue *output) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);XrXirFaultDetail fault={0};
    const XrXirTypes *types=xr_xir_compile_type_arena_types(admission->arena);XrXirValuePath path={step,1};
    if (!xr_xir_type_is_class(types,(XrXirType)value->type)) {
        if (step->kind==XR_XIR_PATH_TUPLE_FIELD || step->kind==XR_XIR_PATH_NULLABLE_VALUE || step->kind==XR_XIR_PATH_ENUM_FIELD)
            return xir_value_observe_child(value,step,admission,output,&fault);
        XrXirValue borrowed=*value;XrXirValuePlace place={(XrXirType)value->type,&borrowed.payload};
        return xr_xir_value_path_read(&place,&path,admission,output,&fault);
    }
    XrXirCallClassAccess access={0};
    XrXirValueStatus status=debug_write_status(xir_instance_debug_class_access(view,value,&access));
    if (status!=XR_XIR_VALUE_OK) return status;
    status=xr_xir_class_authorized_path_read(value,&access.authority,&path,admission,output,&fault);
    xr_xir_call_class_access_finish(&access);return status;
}
static XrXirValueStatus debug_class_write_prepare(XrXirVmDebug *debug,XrXirCallView *view,
    uint32_t function,const XrXirValue *root,uint32_t boundary,DebugClassWrite *output) {
    XrXirValue current={0};XrXirValueStatus status=xr_xir_value_copy(root,&current);
    for (uint32_t i=0;i<debug->write.count && status==XR_XIR_VALUE_OK;++i)
        status=debug_write_member(debug,function,&debug->write.steps[i],i<boundary ? XR_XIR_NOMINAL_READ : XR_XIR_NOMINAL_WRITE);
    for (uint32_t i=0;i<boundary && status==XR_XIR_VALUE_OK;++i) {
        XrXirValue next={0};status=debug_class_write_read(view,&current,&debug->write.steps[i],&next);
        xr_xir_value_drop(&current);current=next;
    }
    if (status!=XR_XIR_VALUE_OK) { xr_xir_value_drop(&current);return status; }
    output->root=current;output->field=debug->write.steps[boundary];
    status=debug_write_status(xir_instance_debug_class_access(view,&output->root,&output->access));
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);XrXirFaultDetail fault={0};XrXirValue before={0};
    XrXirValuePath field={&output->field,1};
    status=xr_xir_class_authorized_path_read(&output->root,&output->access.authority,&field,admission,&before,&fault);
    if (status==XR_XIR_VALUE_OK) {
        XrXirValuePath suffix={debug->write.steps+boundary+1,debug->write.count-boundary-1};
        status=xir_value_path_replacement(&before,&suffix,&debug->write.value,admission,&output->candidate,&fault);
    }
    if (status==XR_XIR_VALUE_OK) status=xr_xir_value_admit(&output->candidate,(XrXirType)before.type,admission);
    xr_xir_value_drop(&before);return status;
}
