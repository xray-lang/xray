/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_path_cwd.inc.c - Authenticated Path query over the shared cwd kernel
 */
static bool instance_path_target(const XrXirProgram *program,uint32_t target) {
    if(!program||target>=program->entry_count||!program->declarations||!program->declarations->functions||
        !program->permissions||!program->permissions->input||!program->permissions->input->artifact||
        !program->permissions->effects||!program->permissions->entries||
        program->permissions->function_count!=program->entry_count||
        !xir_path_native_scope(program->declarations,target,program->entry_count))return false;
    uint32_t id=program->declarations->functions[target].native_callable;
    const XrXirCallEntry *entry=program->entries+target;XrXirEffectFacts facts=xr_xir_effects_read(program->permissions->effects,target);
    return xir_path_native_id(id)&&xir_native_callable_identity(program->declarations->functions+target)&&
        facts.status==XR_XIR_OK&&facts.control.suspend==XR_XIR_EFFECT_NONE&&
        entry->resume==xr_xir_instance_native_callable&&entry->release==xr_xir_instance_native_callable_release&&
        entry->state_bytes==sizeof(XrXirValue)&&!entry->environment&&!entry->flags&&!entry->cleanup_owner&&
        xir_path_native_signature(id,entry->parameters,entry->parameter_count,entry->result,XR_XIR_NEVER);
}
XR_FUNC bool xir_instance_path_nosuspend_transport(XrXirCallView *view,uint32_t target) {
    if(!view||!instance_view_current(view))return false;
    XrXirInstance *instance=view_instance(view);return instance&&instance_path_target(instance->program,target);
}
static XrXirCallStatus instance_path_site(XrXirCallView *view,XrXirInstance *instance,const XirCallSignalOrigin *origin) {
    const XrXirProgram *program=instance->program;
    if(!xr_xir_call_admission(view)||view->phase!=XR_XIR_CALL_NORMAL||view->scope_exit||
        !instance_path_target(program,origin->target))return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=program->permissions->input->artifact;const XrXirModule *module=&artifact->module;
    if(artifact->program_final!=program->permissions->effects||module->stage!=XR_XIR_LOWERED||
        module->function_count!=program->entry_count||!module->functions||origin->caller>=module->function_count||
        origin->instruction>=module->functions[origin->caller].instruction_count||
        !xir_path_native_scope(module->declarations,origin->caller,module->function_count))return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=module->functions[origin->caller].instructions+origin->instruction;
    if(op->op!=XR_XIR_CALL||op->immediate!=(int64_t)origin->target||op->args[1])return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(admission->context!=instance||admission->domain!=instance->domain||admission->arena!=program->arena)return XR_XIR_CALL_BAD_STATE;
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_path_stage(XrXirCallView *view,uint32_t instruction,uint32_t target) {
    XrXirInstance *instance=view_instance(view);if(!instance)return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_path_site(view,instance,&origin);
    return status==XR_XIR_CALL_READY?xir_call_private_native_stage(view,instruction,target):status;
}
static XrXirAction instance_path_resume(XrXirCallView *view,XrXirInstance *instance) {
    XirCallSignalOrigin origin={0};XrXirCallStatus status=xir_call_private_native_origin(view,&origin);
    if(status==XR_XIR_CALL_READY&&origin.target!=xr_xir_call_current_entry(view->activation))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_path_site(view,instance,&origin);
    XrXirValue *value=view->state;
    if(status==XR_XIR_CALL_READY&&(value->type||value->reserved||value->payload))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_current_directory(view,instance,value);
    XrXirAction action={0};if(status==XR_XIR_CALL_READY)status=xr_xir_call_return_move(view,value,&action);
    return status==XR_XIR_CALL_READY?action:instance_pipe_fault(status);
}
