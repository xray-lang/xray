/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io_file.inc.c - Authenticated file boundaries without an OS provider
 */
static bool instance_io_file_target(const XrXirProgram *program,uint32_t target) {
    if(!program || target>=program->entry_count || !program->declarations || !program->declarations->functions ||
        !program->permissions || !program->permissions->input || !program->permissions->input->artifact ||
        !program->permissions->effects || !program->permissions->entries ||
        program->permissions->function_count!=program->entry_count ||
        !xir_io_native_module_scope(program->declarations,target,program->entry_count))return false;
    uint32_t id=program->declarations->functions[target].native_callable;
    const XrXirCallEntry *entry=program->entries+target;
    XrXirEffectFacts facts=xr_xir_effects_read(program->permissions->effects,target);
    return xir_io_file_id(id) && xir_native_callable_identity(program->declarations->functions+target) &&
        facts.status==XR_XIR_OK && facts.control.suspend==(xir_io_native_can_suspend(id)?XR_XIR_EFFECT_MAY:XR_XIR_EFFECT_NONE) &&
        !facts.root.requires_root && !facts.root.unresolved &&
        entry->resume==xr_xir_instance_native_callable && entry->release==xr_xir_instance_native_callable_release &&
        entry->state_bytes==sizeof(XrXirValue) && !entry->environment && !entry->flags && !entry->cleanup_owner &&
        xir_io_native_signature(id,program->types,entry->parameters,entry->parameter_count,entry->result,XR_XIR_NEVER);
}
XR_FUNC bool xir_instance_io_file_nosuspend_transport(XrXirCallView *view,uint32_t target) {
    if(!view || !instance_view_current(view))return false;
    XrXirInstance *instance=view_instance(view);
    return instance && instance_io_file_target(instance->program,target) &&
        instance->program->declarations->functions[target].native_callable==XR_NATIVE_CALLABLE_IO_FILE_CLOSE;
}
static XrXirCallStatus instance_io_file_site(XrXirCallView *view,XrXirInstance *instance,
    const XirCallSignalOrigin *origin,const XrXirValue *arguments,uint32_t count) {
    const XrXirProgram *program=instance->program;
    if(!xr_xir_call_admission(view) || view->phase!=XR_XIR_CALL_NORMAL || view->scope_exit ||
        !instance_io_file_target(program,origin->target))return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=program->permissions->input->artifact;const XrXirModule *module=&artifact->module;
    if(artifact->program_final!=program->permissions->effects || module->stage!=XR_XIR_LOWERED ||
        module->function_count!=program->entry_count || !module->functions || origin->caller>=module->function_count ||
        origin->instruction>=module->functions[origin->caller].instruction_count ||
        !xir_io_native_module_scope(module->declarations,origin->caller,module->function_count))return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=module->functions[origin->caller].instructions+origin->instruction;
    const XrXirCallEntry *entry=program->entries+origin->target;
    uint32_t id=program->declarations->functions[origin->target].native_callable;
    if(op->op!=XR_XIR_CALL || op->immediate!=(int64_t)origin->target || op->args[1]!=count ||
        count!=entry->parameter_count || !arguments ||
        (xir_io_native_can_suspend(id) && xr_xir_call_cleanup_active(view->activation)))return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(admission->context!=instance || admission->domain!=instance->domain || admission->arena!=program->arena)
        return XR_XIR_CALL_BAD_STATE;
    for(uint32_t p=0;p<count;++p) {
        XrXirCallStatus status=admit_instance_value(admission,arguments+p,entry->parameters[p]);
        if(status!=XR_XIR_CALL_READY)return status;
    }
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_io_file_stage(XrXirCallView *view,uint32_t instruction,uint32_t target,
    const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);if(!instance)return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_io_file_site(view,instance,&origin,arguments,count);
    if(status!=XR_XIR_CALL_READY)return status;
    return xir_io_native_can_suspend(instance->program->declarations->functions[target].native_callable)?
        xir_call_signal_stage(view,instruction,target):xir_call_private_native_stage(view,instruction,target);
}
static XrXirAction instance_io_file_action(XrXirCallView *view,XrXirInstance *instance,uint32_t id) {
    XirCallSignalOrigin origin={0};
    XrXirCallStatus status=xir_io_native_can_suspend(id)?xir_call_signal_origin(view,&origin):
        xir_call_private_native_origin(view,&origin);
    uint32_t target=xr_xir_call_current_entry(view->activation);
    if(status==XR_XIR_CALL_READY && origin.target!=target)status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_io_file_site(view,instance,&origin,view->arguments,view->argument_count);
    XrXirValue *output=view->state;
    if(status==XR_XIR_CALL_READY && (!output || !value_unit(*output)))status=XR_XIR_CALL_BAD_STATE;
    uint32_t fields[2]={0};
    if(status==XR_XIR_CALL_READY)status=instance_io_permission(view,instance,target,fields);
    if(status==XR_XIR_CALL_READY)status=XR_XIR_CALL_UNSUPPORTED;
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,status}};
}
