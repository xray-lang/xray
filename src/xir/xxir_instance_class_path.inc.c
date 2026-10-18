/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_class_path.inc.c - Immediate paths keep one authentic Class loan
 */
typedef struct InstanceClassPathOutput {
    const XrXirValue *replacement;
    XrXirValue *output;
    XrXirFaultDetail *fault;
} InstanceClassPathOutput;
static XrXirCallStatus instance_class_path_operate(XrXirCallView *view,uint32_t instruction,
    XirEffectClassAccessKind kind,const XrXirValueReceiver *receiver,
    const XrXirValuePath *path,const InstanceClassPathOutput *operation) {
    XrXirInstance *instance=NULL;
    XrXirCallStatus owner_status=instance_class_view(view,&instance);
    if (owner_status!=XR_XIR_CALL_READY) return owner_status;
    if (!instance || !receiver || !path || !operation || !operation->fault ||
        (operation->output && !value_unit(*operation->output))) return XR_XIR_CALL_BAD_STATE;
    *operation->fault=(XrXirFaultDetail){0};
    if (!instance_class_root(instance,&receiver->value)) {
        switch (kind) {
        case XIR_EFFECT_ACCESS_PATH_READ:
            return xr_xir_instance_path_read(view,receiver,path,operation->output,operation->fault);
        case XIR_EFFECT_ACCESS_LENGTH:
            return xr_xir_instance_path_length(view,receiver,path,operation->output,operation->fault);
        case XIR_EFFECT_ACCESS_CAPACITY:
            return xr_xir_instance_path_capacity(view,receiver,path,operation->output,operation->fault);
        case XIR_EFFECT_ACCESS_PUSH:
            return xr_xir_instance_path_push(view,receiver,path,operation->replacement,operation->fault);
        case XIR_EFFECT_ACCESS_PATH_WRITE:
            return xr_xir_instance_path_write(view,receiver,path,operation->replacement,operation->fault);
        default: return XR_XIR_CALL_BAD_STATE;
        }
    }
    bool cell=receiver->kind==XR_XIR_ROOT_CELL;
    if ((!cell && receiver->kind!=XR_XIR_ROOT_OBJECT) || receiver->local_payload || receiver->slot ||
        (!cell && !xr_xir_value_argument(&receiver->value,instance->program->arena,receiver->type)) ||
        (cell && !xir_type_arena_same_type(xr_xir_value_arena(&receiver->value),
            xr_xir_cell_element(xr_xir_compile_type_arena_types(xr_xir_value_arena(&receiver->value)),
                (XrXirType)receiver->value.type),instance->program->arena,receiver->type)))
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallClassAccess access={0};
    XrXirCallStatus status=instance_class_access_begin(view,instruction,kind,&receiver->value,&access);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirValue array={0};
    bool measure=kind==XIR_EFFECT_ACCESS_LENGTH || kind==XIR_EFFECT_ACCESS_CAPACITY;
    XrXirValue *output=measure?&array:operation->output;
    XrXirValueStatus result;
    if (output) result=cell?xr_xir_call_cell_path_read(view,&receiver->value,path,output,operation->fault):
        xr_xir_class_authorized_path_read(&receiver->value,&access.authority,path,admission,output,operation->fault);
    else if (cell) result=xr_xir_call_cell_path_write(view,&receiver->value,path,
        operation->replacement,operation->fault,kind==XIR_EFFECT_ACCESS_PUSH);
    else result=kind==XIR_EFFECT_ACCESS_PUSH?
        xr_xir_class_authorized_path_push(&receiver->value,&access.authority,path,
            operation->replacement,admission,operation->fault):
        xr_xir_class_authorized_path_write(&receiver->value,&access.authority,path,
            operation->replacement,admission,operation->fault);
    int64_t number=0;
    if (result==XR_XIR_VALUE_OK && measure) result=kind==XIR_EFFECT_ACCESS_LENGTH?
        xr_xir_array_len(&array,admission,&number):xr_xir_array_capacity(&array,admission,&number);
    xr_xir_value_drop(&array);
    status=value_call_status(result);
    xr_xir_call_class_access_finish(&access);
    if (status==XR_XIR_CALL_READY && measure) *operation->output=(XrXirValue){XR_XIR_I64,0,number};
    return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_read_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!output) return XR_XIR_CALL_BAD_ARGUMENT;
    InstanceClassPathOutput operation={NULL,output,fault};
    return instance_class_path_operate(view,instruction,XIR_EFFECT_ACCESS_PATH_READ,receiver,path,&operation);
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_write_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,const XrXirValue *value,XrXirFaultDetail *fault) {
    if (!value) return XR_XIR_CALL_BAD_ARGUMENT;
    InstanceClassPathOutput operation={value,NULL,fault};
    return instance_class_path_operate(view,instruction,XIR_EFFECT_ACCESS_PATH_WRITE,receiver,path,&operation);
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_push_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,const XrXirValue *value,XrXirFaultDetail *fault) {
    if (!value) return XR_XIR_CALL_BAD_ARGUMENT;
    InstanceClassPathOutput operation={value,NULL,fault};
    return instance_class_path_operate(view,instruction,XIR_EFFECT_ACCESS_PUSH,receiver,path,&operation);
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_length_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!output) return XR_XIR_CALL_BAD_ARGUMENT;
    InstanceClassPathOutput operation={NULL,output,fault};
    return instance_class_path_operate(view,instruction,XIR_EFFECT_ACCESS_LENGTH,receiver,path,&operation);
}
XR_FUNC XrXirCallStatus xr_xir_instance_path_capacity_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValueReceiver *receiver,const XrXirValuePath *path,XrXirValue *output,XrXirFaultDetail *fault) {
    if (!output) return XR_XIR_CALL_BAD_ARGUMENT;
    InstanceClassPathOutput operation={NULL,output,fault};
    return instance_class_path_operate(view,instruction,XIR_EFFECT_ACCESS_CAPACITY,receiver,path,&operation);
}
