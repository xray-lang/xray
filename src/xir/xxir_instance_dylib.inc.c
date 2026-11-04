/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_dylib.inc.c - Authenticated loader calls and readonly external values
 */
static bool instance_dylib_target(const XrXirProgram *program,uint32_t target) {
    if(!program||target>=program->entry_count||!program->declarations||!program->declarations->functions||
        !program->permissions||!program->permissions->input||!program->permissions->input->artifact||
        !program->permissions->effects||!program->permissions->entries||
        program->permissions->function_count!=program->entry_count||
        !xir_sys_native_scope(program->declarations,target,program->entry_count))return false;
    uint32_t id=program->declarations->functions[target].native_callable;
    const XrXirCallEntry *entry=program->entries+target;
    XrXirEffectFacts facts=xr_xir_effects_read(program->permissions->effects,target);
    return xir_sys_dylib_id(id)&&xir_native_callable_identity(program->declarations->functions+target)&&
        facts.status==XR_XIR_OK&&facts.control.suspend==XR_XIR_EFFECT_NONE&&
        entry->resume==xr_xir_instance_native_callable&&entry->release==xr_xir_instance_native_callable_release&&
        entry->state_bytes==sizeof(XrXirValue)&&!entry->environment&&!entry->flags&&!entry->cleanup_owner&&
        xir_sys_dylib_signature(id,program->types,entry->parameters,entry->parameter_count,entry->result,XR_XIR_NEVER);
}
XR_FUNC bool xir_instance_dylib_nosuspend_transport(XrXirCallView *view,uint32_t target) {
    if(!view||!instance_view_current(view))return false;
    XrXirInstance *instance=view_instance(view);
    return instance&&instance_dylib_target(instance->program,target);
}
static XrXirCallStatus instance_dylib_site(XrXirCallView *view,XrXirInstance *instance,
    const XirCallSignalOrigin *origin,const XrXirValue *arguments,uint32_t count) {
    const XrXirProgram *program=instance->program;
    if(!xr_xir_call_admission(view)||view->phase!=XR_XIR_CALL_NORMAL||view->scope_exit||
        !instance_dylib_target(program,origin->target))return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=program->permissions->input->artifact;
    const XrXirModule *module=&artifact->module;
    if(artifact->program_final!=program->permissions->effects||module->stage!=XR_XIR_LOWERED||
        module->function_count!=program->entry_count||!module->functions||origin->caller>=module->function_count||
        origin->instruction>=module->functions[origin->caller].instruction_count)return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=module->functions[origin->caller].instructions+origin->instruction;
    const XrXirCallEntry *entry=program->entries+origin->target;
    if(op->op!=XR_XIR_CALL||op->immediate!=(int64_t)origin->target||op->args[1]!=count||count!=entry->parameter_count||
        (count&&!arguments))return XR_XIR_CALL_BAD_STATE;
    if(program->declarations->functions[origin->target].native_callable==XR_NATIVE_CALLABLE_SYS_DYLIB_OPEN&&
        (instance->stopping||instance_root(instance)->stopping))return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(admission->context!=instance||admission->domain!=instance->domain||admission->arena!=program->arena)return XR_XIR_CALL_BAD_STATE;
    for(uint32_t p=0;p<count;++p) {
        XrXirCallStatus status=admit_instance_value(admission,arguments+p,entry->parameters[p]);
        if(status!=XR_XIR_CALL_READY)return status;
    }
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_dylib_stage(XrXirCallView *view,uint32_t instruction,uint32_t target,
    const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);if(!instance)return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_dylib_site(view,instance,&origin,arguments,count);
    return status==XR_XIR_CALL_READY?xir_call_private_native_stage(view,instruction,target):status;
}
static XrXirCallStatus instance_dylib_frame_new(XrXirCallView *view,XrXirInstance *instance,uint32_t id,XirDylibFrame **output) {
    XirCallSignalOrigin origin={0};XrXirCallStatus status=xir_call_private_native_origin(view,&origin);
    if(status!=XR_XIR_CALL_READY)return status;
    uint32_t entry=xr_xir_call_current_entry(view->activation);if(origin.target!=entry)return XR_XIR_CALL_BAD_STATE;
    status=instance_dylib_site(view,instance,&origin,view->arguments,view->argument_count);
    if(status!=XR_XIR_CALL_READY)return status;
    XrXirInstance *root=instance_root(instance);
    if(!instance_dylib_owners_close(root,false))return XR_XIR_CALL_HOST_ERROR;
    XirDylibFrame *frame=instance_metadata_allocate(root->domain,sizeof(*frame),&status);if(!frame)return status;
    *frame=(XirDylibFrame){.next=root->dylib_frames,.call=view->activation,.frame=view->state,
        .generation=instance,.generation_id=instance->generation_id,.entry=entry,.id=id,.origin=origin};
    root->dylib_frames=frame;*output=frame;return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_dylib_path(XrXirCallView *view,XrXirInstance *instance,uint32_t entry,XrXirValue *output) {
    const XrXirModule *module=instance_method_module(instance);XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!module||!module->types||!module->types->nominals||!module->declarations||entry>=module->function_count||
        !admission||admission->domain!=instance->domain||admission->arena!=view->arena)return XR_XIR_CALL_BAD_STATE;
    const XrXirFunction *function=module->functions+entry;uint32_t field=0;
    if(function->parameter_count!=1||!function->parameters||function->parameters[0]!=instance->program->entries[entry].parameters[0])
        return XR_XIR_CALL_BAD_STATE;
    const XrXirTypeNode *path=xr_xir_type_node(module->types,function->parameters[0]);
    if(!path||path->kind!=XR_XIR_TYPE_NOMINAL)return XR_XIR_CALL_BAD_STATE;
    uint64_t work=32u+8u*((uint64_t)module->types->nominals->count+module->declarations->module_count+path->nominal.field_count);
    if(work>admission->work||!xr_xir_domain_work(instance->domain,work))return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    if(xir_native_callable_verify(module,entry)!=XR_XIR_OK||
        !xir_sys_dylib_path_scope(module->declarations,module->types,entry,module->function_count,function->parameters[0],&field)||
        xr_xir_compile_nominal_access(&instance->program->context,module,entry,function->parameters[0],field,XR_XIR_NOMINAL_READ)!=XR_XIR_OK)
        return XR_XIR_CALL_BAD_STATE;
    XrXirValue base={0};
    if(!xr_xir_value_from_payload(view->arena,function->parameters[0],view->arguments[0].payload,&base))return XR_XIR_CALL_BAD_ARGUMENT;
    return value_call_status(xr_xir_class_get(&base,field,admission,output));
}
static XrXirCallStatus instance_dylib_open_value(XrXirCallView *view,XrXirInstance *instance,XirDylibFrame *frame) {
    XrXirValue text={0};XrXirCallStatus status=instance_dylib_path(view,instance,frame->entry,&text);
    XrOsProcessText span={0};if(status==XR_XIR_CALL_READY)status=instance_process_text(view,&text,&span);
    XrXirInstance *root=instance_root(instance);
    if(status==XR_XIR_CALL_READY) {
        XirDylibOwner *node=instance_metadata_allocate(root->domain,sizeof(*node),&status);
        if(node) {
            *node=(XirDylibOwner){.next=root->dylib_owners,.generation=instance->generation_id,.references=1};
            root->dylib_owners=node;frame->library=node;
            status=instance_owned_tokens(1,&node->token);
            if(status==XR_XIR_CALL_READY)status=value_call_status(xir_pointer_origin_create(instance->domain,&node->origin));
            XirPipeQuery current={view,instance->domain,status};XrOsDylibQuery query={&current,instance_pipe_current};
            XrOsIoPolicy memory={root->domain,instance_pipe_allocate,instance_pipe_free};
            if(status==XR_XIR_CALL_READY)status=instance_pipe_status(xr_os_io_dylib_open(&memory,&query,
                (XrOsDylibText){span.text,span.length},&node->owner,&frame->opened));
            if(!instance_pipe_current(&current))status=current.status;
        }
    }
    xr_xir_value_drop(&text);
    if(status==XR_XIR_CALL_READY)*frame->frame=(XrXirValue){XR_XIR_I64,0,frame->opened?frame->library->token:0};
    return status;
}
static XrXirCallStatus instance_dylib_symbol_value(XrXirCallView *view,XrXirInstance *instance,
    XirDylibFrame *frame,XrXirType type) {
    XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};
    XirDylibOwner *node=instance_dylib_lookup(instance,view->arguments[0].payload,&current);
    XrXirCallStatus status=current.status;XrXirValue pointer={0};bool found=false;uintptr_t address=0;
    if(node&&!node->closing) {
        if(node->references==UINT64_MAX)return XR_XIR_CALL_LIMIT;
        frame->library=node;++node->references;
        XrOsProcessText name={0};if(status==XR_XIR_CALL_READY)status=instance_process_text(view,view->arguments+1,&name);
        XrOsDylibQuery query={&current,instance_pipe_current};
        if(status==XR_XIR_CALL_READY)status=instance_pipe_status(xr_os_io_dylib_symbol(&node->owner,&query,
            (XrOsDylibText){name.text,name.length},&address,&found));
        if(!instance_pipe_current(&current))status=current.status;
        if(status==XR_XIR_CALL_READY&&found)status=value_call_status(xir_pointer_origin_readonly(node->origin,view->arena,
            xr_xir_nullable_element(instance->program->types,type),address,&pointer));
    }
    if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_nullable_new(type,found?&pointer:NULL,
        xr_xir_call_admission(view),frame->frame));
    xr_xir_value_drop(&pointer);return status;
}
static XrXirCallStatus instance_dylib_error_value(XrXirCallView *view,XrXirInstance *instance,XrXirValue *output) {
    XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};XrOsDylibQuery query={&current,instance_pipe_current};
    XrOsIoPolicy memory={instance->domain,instance_pipe_allocate,instance_pipe_free};XrOsDylibErrorText text={0};
    XrXirCallStatus status=instance_pipe_status(xr_os_io_dylib_last_error(&memory,&query,&text));
    if(!instance_pipe_current(&current))status=current.status;
    if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_string_new(instance->domain,text.text,text.length,output));
    xr_os_io_dylib_error_free(&text);return status;
}
static XrXirCallStatus instance_dylib_commit(void *context) {
    XirDylibFrame *frame=context;
    if(frame->opened)frame->library->published=true;
    if(frame->closed&&frame->library)frame->library->consumed=true;
    return XR_XIR_CALL_READY;
}
static XrXirAction instance_dylib_resume(XrXirCallView *view,XrXirInstance *instance,uint32_t id,const XrXirCallEntry *entry) {
    if(!value_unit(*(XrXirValue *)view->state))return instance_pipe_fault(XR_XIR_CALL_BAD_STATE);
    XirDylibFrame *frame=instance_dylib_frame_find(instance_root(instance),view);XrXirCallStatus status=XR_XIR_CALL_READY;
    if(!frame)status=instance_dylib_frame_new(view,instance,id,&frame);
    else if(frame->generation!=instance||frame->generation_id!=instance->generation_id||frame->id!=id||
        frame->entry!=xr_xir_call_current_entry(view->activation))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_dylib_site(view,instance,&frame->origin,view->arguments,view->argument_count);
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    if(id==XR_NATIVE_CALLABLE_SYS_DYLIB_OPEN)status=instance_dylib_open_value(view,instance,frame);
    else if(id==XR_NATIVE_CALLABLE_SYS_DYLIB_SYMBOL)status=instance_dylib_symbol_value(view,instance,frame,entry->result);
    else if(id==XR_NATIVE_CALLABLE_SYS_DYLIB_LAST_ERROR)status=instance_dylib_error_value(view,instance,frame->frame);
    else {
        XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};
        XirDylibOwner *node=instance_dylib_lookup(instance,view->arguments[0].payload,&current);status=current.status;
        if(node) {
            if(node->references==UINT64_MAX)return instance_pipe_fault(XR_XIR_CALL_LIMIT);
            frame->library=node;++node->references;
            if(status==XR_XIR_CALL_READY)status=instance_pipe_status(instance_dylib_close_owner(node,&frame->closed));
            if(!instance_pipe_current(&current))status=current.status;
        }
        if(status==XR_XIR_CALL_READY)*frame->frame=(XrXirValue){XR_XIR_BOOL,0,frame->closed?1:0};
    }
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    XrXirAction action={0};status=xir_call_return_move_commit(view,frame->frame,&action,instance_dylib_commit,frame);
    return status==XR_XIR_CALL_READY?action:instance_pipe_fault(status);
}
