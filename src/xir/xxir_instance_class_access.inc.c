/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_class_access.inc.c - Genuine instruction access under a Class loan
 */
/* A host Call owns an opaque context. Its live admission must name the
 * private Instance gate before any Instance or Domain field is inspected. */
static XrXirCallStatus instance_class_view(XrXirCallView *view,XrXirInstance **output) {
    if (!view || !output || *output) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if (!admission || admission->function!=admit_function_binding) return XR_XIR_CALL_BAD_STATE;
    XrXirInstance *instance=admission->context;
    if (!instance || instance_root(instance)!=view->instance) return XR_XIR_CALL_BAD_STATE;
    if (!instance_owner_current(instance)) return XR_XIR_CALL_BAD_ARGUMENT;
    if (admission->domain!=instance->domain || view_instance(view)!=instance ||
        admission->arena!=instance->program->arena) return XR_XIR_CALL_BAD_STATE;
    *output=instance;
    return XR_XIR_CALL_READY;
}
static const XrXirValue *instance_class_root(const XrXirInstance *instance,const XrXirValue *value) {
    if (!value) return NULL;
    const XrXirValue *root=xr_xir_type_is_cell(instance->program->types,(XrXirType)value->type)?
        xr_xir_cell_canonical_root(value):value;
    return root && xr_xir_type_is_class(instance->program->types,(XrXirType)root->type)?root:NULL;
}
static XrXirCallStatus instance_class_access_begin(XrXirCallView *view,uint32_t instruction,
    XirEffectClassAccessKind kind,const XrXirValue *value,XrXirCallClassAccess *access) {
    XrXirInstance *instance=view_instance(view);
    if (!instance || !value || !access) return XR_XIR_CALL_BAD_STATE;
    const XrXirValue *root=instance_class_root(instance,value);
    if (!root) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirCallStatus status=admit_instance_value(admission,value,(XrXirType)value->type);
    if (status!=XR_XIR_CALL_READY) return status;
    status=admit_instance_value(admission,root,(XrXirType)root->type);
    if (status!=XR_XIR_CALL_READY) return status;
    status=value_call_status(xr_xir_call_class_access_prepare(view,value,access));
    if (status!=XR_XIR_CALL_READY) return status;
    /* PREPARED now excludes publication. BORROWED keeps it excluded through
     * the complete read/write. RC and graph operation leases grant no access. */
    bool pure=view_is_child(instance,view);
    if (pure && xr_xir_class_module_storage(root)) status=XR_XIR_CALL_BAD_STATE;
    const XirEffectInvocationSelection *current=xr_xir_call_invocation(view);
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    if (status==XR_XIR_CALL_READY && current &&
        (current->owner!=instance->program->permissions->effects || current->target!=caller))
        status=XR_XIR_CALL_BAD_STATE;
    if (status==XR_XIR_CALL_READY) {
        InstanceInvocationRead actual={admission,NULL,view,NULL};
        XirEffectClassAccessRequest request={.caller=caller,.instruction=instruction,
            .node=current?current->node:UINT32_MAX,.contextual=current!=NULL,.pure=pure,
            .kind=kind,.input_type=(XrXirType)value->type,.root_type=(XrXirType)root->type,
            .read={.owner=&actual,.work=instance_invocation_work}};
        XrXirStatus checked=xir_effects_class_access(instance->program->permissions->effects,&request);
        status=checked==XR_XIR_OK?XR_XIR_CALL_READY:checked==XR_XIR_BUDGET?
            XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_STATE;
    }
    if (status==XR_XIR_CALL_READY) xr_xir_call_class_access_commit(view,access);
    else xr_xir_call_class_access_finish(access);
    return status;
}
static XrXirCallStatus instance_class_scope_access(XrXirCallView *view,
    const XrXirValue *value,XrXirCallClassAccess *access,bool control) {
    XrXirInstance *instance=NULL;
    XrXirCallStatus status=instance_class_view(view,&instance);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!value || !access || !instance->driving || !xir_call_vm_view_current(view) ||
        xr_xir_call_cleanup_active(view->activation) || (control && !instance->debug_visiting)) return XR_XIR_CALL_BAD_STATE;
    const XrXirValue *root=instance_class_root(instance,value);
    if (!root) return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    status=admit_instance_value(admission,value,(XrXirType)value->type);
    if (status==XR_XIR_CALL_READY && root!=value) status=admit_instance_value(admission,root,(XrXirType)root->type);
    if (status!=XR_XIR_CALL_READY) return status;
    status=value_call_status(xr_xir_call_class_access_prepare(view,value,access));
    if (status!=XR_XIR_CALL_READY) return status;
    bool pure=view_is_child(instance,view);
    if (pure && xr_xir_class_module_storage(root)) status=XR_XIR_CALL_BAD_STATE;
    const XirEffectInvocationSelection *current=xr_xir_call_invocation(view);
    uint32_t caller=xr_xir_call_current_entry(view->activation);
    if (status==XR_XIR_CALL_READY && current &&
        (current->owner!=instance->program->permissions->effects || current->target!=caller)) status=XR_XIR_CALL_BAD_STATE;
    if (status==XR_XIR_CALL_READY) {
        InstanceInvocationRead actual={admission,NULL,view,NULL};
        XirEffectClassScopeRequest request={.caller=caller,.node=current?current->node:UINT32_MAX,
            .contextual=current!=NULL,.pure=pure,.root_type=(XrXirType)root->type,
            .read={.owner=&actual,.work=instance_invocation_work}};
        XrXirStatus checked=xir_effects_class_scope(instance->program->permissions->effects,&request);
        status=checked==XR_XIR_OK?XR_XIR_CALL_READY:checked==XR_XIR_BUDGET?XR_XIR_CALL_LIMIT:XR_XIR_CALL_BAD_STATE;
    }
    if (status==XR_XIR_CALL_READY) xr_xir_call_class_access_commit(view,access);
    else xr_xir_call_class_access_finish(access);
    return status;
}
XR_FUNC XrXirCallStatus xir_instance_debug_class_access(XrXirCallView *view,
    const XrXirValue *value,XrXirCallClassAccess *access) {
    return instance_class_scope_access(view,value,access,true);
}
XR_FUNC XrXirCallStatus xir_instance_observe_cell(XrXirCallView *view,
    const XrXirValue *cell,XrXirValue *output) {
    XrXirInstance *instance=NULL;XrXirCallStatus status=instance_class_view(view,&instance);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!cell || !output || !value_unit(*output) || !xr_xir_type_is_cell(instance->program->types,(XrXirType)cell->type) || !xir_call_vm_view_current(view) ||
        xr_xir_call_cleanup_active(view->activation)) return XR_XIR_CALL_BAD_STATE;
    if (!instance_class_root(instance,cell)) return xr_xir_instance_cell_read(view,cell,output);
    XrXirCallClassAccess access={0};status=instance_class_scope_access(view,cell,&access,false);
    if (status!=XR_XIR_CALL_READY) return status;
    status=value_call_status(xr_xir_call_cell_read(view,cell,output));
    xr_xir_call_class_access_finish(&access);return status;
}
XR_FUNC XrXirCallStatus xir_instance_debug_cell_write(XrXirCallView *view,
    const XrXirValue *cell,const XrXirValue *value) {
    XrXirInstance *instance=NULL;XrXirCallStatus status=instance_class_view(view,&instance);
    if (status!=XR_XIR_CALL_READY) return status;
    if (!cell || !value || !xr_xir_type_is_cell(instance->program->types,(XrXirType)cell->type) || !instance->debug_visiting || !xir_call_vm_view_current(view) ||
        xr_xir_call_cleanup_active(view->activation)) return XR_XIR_CALL_BAD_STATE;
    if (!instance_class_root(instance,cell)) return xr_xir_instance_cell_write(view,cell,value);
    XrXirCallClassAccess access={0};status=instance_class_scope_access(view,cell,&access,true);
    if (status!=XR_XIR_CALL_READY) return status;
    status=value_call_status(xr_xir_call_cell_write(view,cell,value));
    xr_xir_call_class_access_finish(&access);return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_cell_read_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *cell,XrXirValue *output,XrXirFaultDetail *fault) {
    XrXirInstance *instance=NULL;
    XrXirCallStatus owner_status=instance_class_view(view,&instance);
    if (owner_status!=XR_XIR_CALL_READY) return owner_status;
    if (!instance || !fault || !output || !value_unit(*output)) return XR_XIR_CALL_BAD_STATE;
    *fault=(XrXirFaultDetail){0};
    if (!instance_class_root(instance,cell)) return xr_xir_instance_cell_read(view,cell,output);
    XrXirCallClassAccess access={0};
    XrXirCallStatus status=instance_class_access_begin(view,instruction,XIR_EFFECT_ACCESS_CELL_READ,cell,&access);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirValuePath path={0};
    status=value_call_status(xr_xir_call_cell_path_read(view,cell,&path,output,fault));
    xr_xir_call_class_access_finish(&access);return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_cell_write_at(XrXirCallView *view,uint32_t instruction,
    const XrXirValue *cell,const XrXirValue *value,XrXirFaultDetail *fault) {
    XrXirInstance *instance=NULL;
    XrXirCallStatus owner_status=instance_class_view(view,&instance);
    if (owner_status!=XR_XIR_CALL_READY) return owner_status;
    if (!instance || !fault || !value) return XR_XIR_CALL_BAD_STATE;
    *fault=(XrXirFaultDetail){0};
    if (!instance_class_root(instance,cell)) return xr_xir_instance_cell_write(view,cell,value);
    XrXirCallClassAccess access={0};
    XrXirCallStatus status=instance_class_access_begin(view,instruction,XIR_EFFECT_ACCESS_CELL_WRITE,cell,&access);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirValuePath path={0};
    status=value_call_status(xr_xir_call_cell_path_write(view,cell,&path,value,fault,false));
    xr_xir_call_class_access_finish(&access);return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_cell_project_at(XrXirCallView *view,uint32_t instruction,
    XrXirType type,const XrXirValueReceiver *receiver,const XrXirValuePath *path,XrXirValue *output) {
    XrXirInstance *instance=NULL;
    XrXirCallStatus owner_status=instance_class_view(view,&instance);
    if (owner_status!=XR_XIR_CALL_READY) return owner_status;
    if (!instance || !receiver || !output || !value_unit(*output) || !path)
        return XR_XIR_CALL_BAD_STATE;
    if (!instance_class_root(instance,&receiver->value))
        return xr_xir_instance_cell_project(view,type,receiver,path,output);
    if ((receiver->kind!=XR_XIR_ROOT_OBJECT && receiver->kind!=XR_XIR_ROOT_CELL) ||
        receiver->local_payload || receiver->slot ||
        (receiver->kind==XR_XIR_ROOT_OBJECT && receiver->value.type!=(uint32_t)receiver->type) ||
        (receiver->kind==XR_XIR_ROOT_CELL &&
            xr_xir_cell_element(instance->program->types,(XrXirType)receiver->value.type)!=receiver->type))
        return XR_XIR_CALL_BAD_STATE;
    XrXirCallClassAccess access={0};
    XrXirCallStatus status=instance_class_access_begin(view,instruction,XIR_EFFECT_ACCESS_PROJECT,
        &receiver->value,&access);
    if (status!=XR_XIR_CALL_READY) return status;
    status=value_call_status(xr_xir_call_cell_project(view,type,&receiver->value,path,output));
    xr_xir_call_class_access_finish(&access);return status;
}
