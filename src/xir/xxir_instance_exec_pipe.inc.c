/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_exec_pipe.inc.c - Canonical shell-capture endpoint cleanup
 */
static bool instance_exec_pipe_target(const XrXirProgram *program,uint32_t target) {
    if(!program||target>=program->entry_count||!program->declarations||!program->declarations->functions||
        !program->permissions||!program->permissions->input||!program->permissions->input->artifact||
        !program->permissions->effects||!program->permissions->entries||
        program->permissions->function_count!=program->entry_count||
        !xir_os_native_scope(program->declarations,target,program->entry_count))return false;
    uint32_t id=program->declarations->functions[target].native_callable;
    const XrXirCallEntry *entry=program->entries+target;
    XrXirEffectFacts facts=xr_xir_effects_read(program->permissions->effects,target);
    return xir_os_exec_close_id(id)&&xir_native_callable_identity(program->declarations->functions+target)&&
        facts.status==XR_XIR_OK&&facts.control.suspend==XR_XIR_EFFECT_NONE&&
        !facts.root.requires_root&&!facts.root.unresolved&&
        entry->resume==xr_xir_instance_native_callable&&entry->release==xr_xir_instance_native_callable_release&&
        entry->state_bytes==sizeof(XrXirValue)&&!entry->environment&&!entry->flags&&!entry->cleanup_owner&&
        xir_os_exec_native_signature(id,entry->parameters,entry->parameter_count,entry->result,XR_XIR_NEVER);
}
XR_FUNC bool xir_instance_exec_pipe_nosuspend_transport(XrXirCallView *view,uint32_t target) {
    if(!view||!instance_view_current(view))return false;
    XrXirInstance *instance=view_instance(view);
    return instance&&instance_exec_pipe_target(instance->program,target);
}
static XrXirCallStatus instance_exec_pipe_site(XrXirCallView *view,XrXirInstance *instance,
    const XirCallSignalOrigin *origin,const XrXirValue *arguments,uint32_t count) {
    const XrXirProgram *program=instance->program;
    if(!xr_xir_call_admission(view)||view->phase!=XR_XIR_CALL_NORMAL||view->scope_exit||
        !instance_exec_pipe_target(program,origin->target))return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=program->permissions->input->artifact;const XrXirModule *module=&artifact->module;
    if(artifact->program_final!=program->permissions->effects||module->stage!=XR_XIR_LOWERED||
        module->function_count!=program->entry_count||!module->functions||origin->caller>=module->function_count||
        origin->instruction>=module->functions[origin->caller].instruction_count||
        !xir_os_native_scope(module->declarations,origin->caller,module->function_count))return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=module->functions[origin->caller].instructions+origin->instruction;
    if(op->op!=XR_XIR_CALL||op->immediate!=(int64_t)origin->target||op->args[1]!=2||count!=2||!arguments)
        return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(admission->context!=instance||admission->domain!=instance->domain||admission->arena!=program->arena)
        return XR_XIR_CALL_BAD_STATE;
    /* Cleanup transport does not replace Call admission. Child entry still
     * checks its actual conditional ROOT inputs against the retained Final;
     * the scalar native target additionally requires closed no-root facts. */
    for(uint32_t p=0;p<2;++p) {
        XrXirCallStatus status=admit_instance_value(admission,arguments+p,XR_XIR_I64);
        if(status!=XR_XIR_CALL_READY)return status;
    }
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_exec_pipe_stage(XrXirCallView *view,uint32_t instruction,uint32_t target,
    const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);if(!instance)return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_exec_pipe_site(view,instance,&origin,arguments,count);
    return status==XR_XIR_CALL_READY?xir_call_private_native_stage(view,instruction,target):status;
}
static XrXirAction instance_exec_pipe_resume(XrXirCallView *view,XrXirInstance *instance) {
    XirCallSignalOrigin origin={0};XrXirCallStatus status=xir_call_private_native_origin(view,&origin);
    if(status==XR_XIR_CALL_READY&&origin.target!=xr_xir_call_current_entry(view->activation))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_exec_pipe_site(view,instance,&origin,view->arguments,view->argument_count);
    XrXirValue *output=view->state;
    if(status==XR_XIR_CALL_READY&&!value_unit(*output))status=XR_XIR_CALL_BAD_STATE;
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    XirPipeOwner *held[2]={NULL,NULL};bool complete=true;
    for(uint32_t end=0;end<2;++end) {
        int64_t token=view->arguments[end].payload;bool closed=token==-1;
        XrXirCallStatus attempt=XR_XIR_CALL_READY;
        if(!closed)attempt=instance_pipe_close_endpoint(view,instance,token,end,held+end,&closed);
        if(attempt!=XR_XIR_CALL_READY&&status==XR_XIR_CALL_READY)status=attempt;
        complete=complete&&closed;
    }
    for(uint32_t end=0;end<2;++end)if(held[end]) {
        XR_CHECK(held[end]->references,"synchronous endpoint cleanup retains its actual Root owner");
        --held[end]->references;
    }
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    *output=(XrXirValue){XR_XIR_BOOL,0,complete?1:0};
    XrXirAction action={0};status=xr_xir_call_return_move(view,output,&action);
    return status==XR_XIR_CALL_READY?action:instance_pipe_fault(status);
}
