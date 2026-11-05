/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_path_cwd.inc.c - Authenticated Path query over the shared cwd kernel
 */
#include "../os/xfullpath_owned.h"

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
static XrXirCallStatus instance_path_site(XrXirCallView *view,XrXirInstance *instance,const XirCallSignalOrigin *origin,const XrXirValue *arguments,uint32_t count) {
    const XrXirProgram *program=instance->program;
    if(!xr_xir_call_admission(view)||view->phase!=XR_XIR_CALL_NORMAL||view->scope_exit||
        !instance_path_target(program,origin->target))return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=program->permissions->input->artifact;const XrXirModule *module=&artifact->module;
    if(artifact->program_final!=program->permissions->effects||module->stage!=XR_XIR_LOWERED||
        module->function_count!=program->entry_count||!module->functions||origin->caller>=module->function_count||
        origin->instruction>=module->functions[origin->caller].instruction_count||
        !xir_path_native_scope(module->declarations,origin->caller,module->function_count))return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=module->functions[origin->caller].instructions+origin->instruction;
    const XrXirCallEntry *entry=program->entries+origin->target;
    if(op->op!=XR_XIR_CALL||op->immediate!=(int64_t)origin->target||op->args[1]!=count ||
        count!=entry->parameter_count || (count&&!arguments))return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(admission->context!=instance||admission->domain!=instance->domain||admission->arena!=program->arena)return XR_XIR_CALL_BAD_STATE;
    for(uint32_t p=0;p<count;++p) {
        XrXirCallStatus status=admit_instance_value(admission,arguments+p,entry->parameters[p]);
        if(status!=XR_XIR_CALL_READY)return status;
    }
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_path_stage(XrXirCallView *view,uint32_t instruction,uint32_t target,
    const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);if(!instance)return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_path_site(view,instance,&origin,arguments,count);
    return status==XR_XIR_CALL_READY?xir_call_private_native_stage(view,instruction,target):status;
}
static XrXirCallStatus instance_path_fullpath(XrXirCallView *view,XrXirInstance *instance,XrXirValue *output) {
    XrXirValue input={0},result={0};const char *bytes=NULL;size_t length=0;
    if(!xr_xir_value_from_payload(view->arena,XR_XIR_STRING,view->arguments[0].payload,&input) ||
        !xr_xir_string_view(&input,&bytes,&length))return XR_XIR_CALL_BAD_ARGUMENT;
    if(length==SIZE_MAX || !xr_xir_domain_work(instance->domain,length+1))return XR_XIR_CALL_LIMIT;
    if(memchr(bytes,0,length))return XR_XIR_CALL_HOST_ERROR;
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
    void *terminated=NULL;char *raw=NULL;
    XrOsIoStatus called=instance_environment_allocate(&query,length+1,&terminated);
    if(called==XR_OS_IO_OK) {
        memcpy(terminated,bytes,length);((char *)terminated)[length]=0;
        called=xr_os_io_fullpath_owned(&policy,terminated,instance_environment_current,&raw);
    }
    instance_environment_free(&query,terminated);
    XrXirCallStatus status=query.status;
    if(status==XR_XIR_CALL_READY)status=instance_environment_status(called);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY&&!raw)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY) {
        XrXirValueStatus copied=xr_xir_string_new(instance->domain,raw,strlen(raw),&result);
        status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    }
    instance_environment_free(&query,raw);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return status;
}
static XrXirAction instance_path_resume(XrXirCallView *view,XrXirInstance *instance) {
    XirCallSignalOrigin origin={0};XrXirCallStatus status=xir_call_private_native_origin(view,&origin);
    if(status==XR_XIR_CALL_READY&&origin.target!=xr_xir_call_current_entry(view->activation))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_path_site(view,instance,&origin,view->arguments,view->argument_count);
    XrXirValue *value=view->state;
    if(status==XR_XIR_CALL_READY&&(value->type||value->reserved||value->payload))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY) {
        uint32_t id=instance->program->declarations->functions[origin.target].native_callable;
        if(id==XR_NATIVE_CALLABLE_PATH_GETCWD)status=instance_current_directory(view,instance,value);
        else if(id==XR_NATIVE_CALLABLE_PATH_FULLPATH)status=instance_path_fullpath(view,instance,value);
        else {
#if defined(XR_OS_WINDOWS)
            *value=(XrXirValue){XR_XIR_BOOL,0,1};
#else
            *value=(XrXirValue){XR_XIR_BOOL,0,0};
#endif
        }
    }
    XrXirAction action={0};if(status==XR_XIR_CALL_READY)status=xr_xir_call_return_move(view,value,&action);
    return status==XR_XIR_CALL_READY?action:instance_pipe_fault(status);
}
