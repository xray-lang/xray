/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_process.inc.c - Direct process calls with owned start publication
 */
typedef struct XirProcessTexts {
    XrOsProcessText *texts;
    XrXirValue *values;
    size_t count,bytes;
} XirProcessTexts;
static bool instance_process_target(const XrXirProgram *program,uint32_t target) {
    if(!program||target>=program->entry_count||!program->declarations||!program->declarations->functions||
        !program->permissions||!program->permissions->input||!program->permissions->input->artifact||
        !program->permissions->effects||!program->permissions->entries||
        program->permissions->function_count!=program->entry_count||
        !xir_sys_native_scope(program->declarations,target,program->entry_count))return false;
    uint32_t id=program->declarations->functions[target].native_callable;
    const XrXirCallEntry *entry=program->entries+target;
    XrXirEffectFacts facts=xr_xir_effects_read(program->permissions->effects,target);
    return xir_sys_process_id(id)&&xir_native_callable_identity(program->declarations->functions+target)&&
        facts.status==XR_XIR_OK&&facts.control.suspend==XR_XIR_EFFECT_NONE&&
        entry->resume==xr_xir_instance_native_callable&&entry->release==xr_xir_instance_native_callable_release&&
        entry->state_bytes==sizeof(XrXirValue)&&!entry->environment&&!entry->flags&&!entry->cleanup_owner&&
        xir_sys_process_signature(id,program->types,entry->parameters,entry->parameter_count,entry->result,XR_XIR_NEVER);
}
XR_FUNC bool xir_instance_process_nosuspend_transport(XrXirCallView *view,uint32_t target) {
    if(!view||!instance_view_current(view))return false;
    XrXirInstance *instance=view_instance(view);
    return instance&&instance_process_target(instance->program,target);
}
static XrXirCallStatus instance_process_site(XrXirCallView *view,XrXirInstance *instance,
    const XirCallSignalOrigin *origin,const XrXirValue *arguments,uint32_t count) {
    const XrXirProgram *program=instance->program;
    if(!xr_xir_call_admission(view)||view->phase!=XR_XIR_CALL_NORMAL||view->scope_exit||
        !instance_process_target(program,origin->target))return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=program->permissions->input->artifact;
    const XrXirModule *module=&artifact->module;
    if(artifact->program_final!=program->permissions->effects||module->stage!=XR_XIR_LOWERED||
        module->function_count!=program->entry_count||!module->functions||origin->caller>=module->function_count||
        origin->instruction>=module->functions[origin->caller].instruction_count)return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=module->functions[origin->caller].instructions+origin->instruction;
    const XrXirCallEntry *entry=program->entries+origin->target;
    if(op->op!=XR_XIR_CALL||op->immediate!=(int64_t)origin->target||op->args[1]!=count||count!=entry->parameter_count||
        (count&&!arguments))return XR_XIR_CALL_BAD_STATE;
    if(program->declarations->functions[origin->target].native_callable==XR_NATIVE_CALLABLE_SYS_PROCESS_SPAWN&&
        (instance->stopping||instance_root(instance)->stopping))return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(admission->context!=instance||admission->domain!=instance->domain||admission->arena!=program->arena)return XR_XIR_CALL_BAD_STATE;
    for(uint32_t p=0;p<count;++p) {
        XrXirCallStatus status=admit_instance_value(admission,arguments+p,entry->parameters[p]);
        if(status!=XR_XIR_CALL_READY)return status;
    }
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_process_stage(XrXirCallView *view,uint32_t instruction,uint32_t target,
    const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);if(!instance)return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_process_site(view,instance,&origin,arguments,count);
    return status==XR_XIR_CALL_READY?xir_call_private_native_stage(view,instruction,target):status;
}
static XrXirCallStatus instance_process_frame_new(XrXirCallView *view,XrXirInstance *instance,uint32_t id,XirProcessFrame **output) {
    XirCallSignalOrigin origin={0};XrXirCallStatus status=xir_call_private_native_origin(view,&origin);
    if(status!=XR_XIR_CALL_READY)return status;
    uint32_t entry=xr_xir_call_current_entry(view->activation);if(origin.target!=entry)return XR_XIR_CALL_BAD_STATE;
    status=instance_process_site(view,instance,&origin,view->arguments,view->argument_count);
    if(status!=XR_XIR_CALL_READY)return status;
    XrXirInstance *root=instance_root(instance);
    if(!instance_process_owners_close(root,false))return XR_XIR_CALL_HOST_ERROR;
    XirProcessFrame *frame=instance_metadata_allocate(root->domain,sizeof(*frame),&status);if(!frame)return status;
    *frame=(XirProcessFrame){.next=root->process_frames,.call=view->activation,.frame=view->state,
        .generation=instance,.generation_id=instance->generation_id,.entry=entry,.id=id,.origin=origin};
    root->process_frames=frame;*output=frame;return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_process_text(XrXirCallView *view,const XrXirValue *value,XrOsProcessText *output) {
    const char *text=NULL;size_t length=0;
    if(!xr_xir_string_view(value,&text,&length))return XR_XIR_CALL_BAD_ARGUMENT;
    if(length>INT_MAX||!xr_xir_domain_work(xr_xir_call_admission(view)->domain,length+1))return XR_XIR_CALL_LIMIT;
    *output=(XrOsProcessText){text,length};return xr_xir_call_execution_status(view);
}
static void instance_process_texts_drop(XrXirInstance *instance,XirProcessTexts *texts) {
    for(size_t i=0;i<texts->count;++i)xr_xir_value_drop(texts->values+i);
    if(texts->texts)xr_xir_domain_call_deallocate(instance->domain,texts->texts,texts->bytes);
    *texts=(XirProcessTexts){0};
}
static XrXirCallStatus instance_process_texts(XrXirCallView *view,XrXirInstance *instance,
    const XrXirValue *array,XirProcessTexts *output) {
    int64_t length=0;XrXirValueAdmission *admission=xr_xir_call_admission(view);
    XrXirValueStatus sized=xr_xir_array_len(array,admission,&length);
    if(sized!=XR_XIR_VALUE_OK)return value_call_status(sized);
    if(length<0||(uint64_t)length>SIZE_MAX/(sizeof(XrOsProcessText)+sizeof(XrXirValue)))return XR_XIR_CALL_LIMIT;
    size_t count=(size_t)length,bytes=count*(sizeof(XrOsProcessText)+sizeof(XrXirValue));
    if(bytes) {
        XrXirValueStatus allocated=XR_XIR_VALUE_OK;
        output->texts=xr_xir_domain_call_allocate(instance->domain,bytes,&allocated);
        if(!output->texts)return value_call_status(allocated);
        output->bytes=bytes;output->values=(XrXirValue *)(output->texts+count);memset(output->values,0,count*sizeof(*output->values));
    }
    output->count=count;
    for(size_t i=0;i<count;++i) {
        XrXirCallStatus status=xr_xir_call_execution_status(view);if(status!=XR_XIR_CALL_READY)return status;
        XrXirFaultDetail fault={0};XrXirValueStatus copied=xr_xir_array_get(array,(int64_t)i,admission,output->values+i,&fault);
        if(copied!=XR_XIR_VALUE_OK)return value_call_status(copied);
        status=instance_process_text(view,output->values+i,output->texts+i);if(status!=XR_XIR_CALL_READY)return status;
    }
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_process_spawn(XrXirCallView *view,XrXirInstance *instance,XirProcessFrame *frame) {
    XrXirInstance *root=instance_root(instance);XrXirCallStatus status=XR_XIR_CALL_READY;
    XrOsProcessRequest request={0};XirProcessTexts args={0},keys={0},values={0};XrOsProcessText cwd={0};
    bool some[6]={0};const XrXirValue *optional[6]={0};
    for(uint32_t p=0;p<6;++p)
        if(!xr_xir_nullable_view(view->arguments+2+p,some+p,optional+p))return XR_XIR_CALL_BAD_ARGUMENT;
    status=instance_process_text(view,view->arguments,&request.program);
    if(status==XR_XIR_CALL_READY)status=instance_process_texts(view,instance,view->arguments+1,&args);
    if(status==XR_XIR_CALL_READY&&some[0]){status=instance_process_text(view,optional[0],&cwd);request.cwd=&cwd;}
    if(status==XR_XIR_CALL_READY&&some[1])status=instance_process_texts(view,instance,optional[1],&keys);
    if(status==XR_XIR_CALL_READY&&some[2])status=instance_process_texts(view,instance,optional[2],&values);
    bool valid=some[1]==some[2]&&keys.count==values.count;
    XirPipeQuery current={view,instance->domain,status};
    for(uint32_t p=0;p<3&&status==XR_XIR_CALL_READY;++p)if(some[p+3]) {
        uint32_t end=0;XirPipeOwner *pipe=instance_pipe_lookup(root,optional[p+3]->payload,&end,&current);
        status=current.status;
        if(status!=XR_XIR_CALL_READY)break;
        if(!pipe||end!=(p?XR_OS_PIPE_WRITE:XR_OS_PIPE_READ)){valid=false;continue;}
        status=instance_pipe_status(xr_os_io_pipe_borrow(&pipe->owner,(XrOsPipeEnd)end,request.streams+p));
        request.redirected[p]=true;
    }
    if(status==XR_XIR_CALL_READY&&valid) {
        XirProcessOwner *node=instance_metadata_allocate(root->domain,sizeof(*node),&status);
        if(node) {
            *node=(XirProcessOwner){.next=root->process_owners,.generation=instance->generation_id,.references=1,
                .detached=view->arguments[8].payload!=0};root->process_owners=node;frame->process=node;
            status=instance_process_token(&node->token);
            request.arguments=args.texts;request.argument_count=args.count;
            request.env_keys=keys.texts;request.env_values=values.texts;request.environment_count=keys.count;
            request.detached=node->detached;
            XrOsIoPolicy memory={root->domain,instance_pipe_allocate,instance_pipe_free};
            XrOsProcessQuery query={&current,instance_pipe_current};current.status=status;
            if(status==XR_XIR_CALL_READY)status=instance_pipe_status(xr_os_io_process_prepare(&memory,&query,&request,&node->owner,&frame->created));
            if(!instance_pipe_current(&current))status=current.status;
        }
    }
    instance_process_texts_drop(instance,&values);instance_process_texts_drop(instance,&keys);instance_process_texts_drop(instance,&args);
    if(status==XR_XIR_CALL_READY)*frame->frame=(XrXirValue){XR_XIR_I64,0,frame->created?frame->process->token:-1};
    return status;
}
static XrXirCallStatus instance_process_commit(void *context) {
    XirProcessFrame *frame=context;
    if(frame->created) {
        XrXirCallStatus status=instance_pipe_status(xr_os_io_process_start(&frame->process->owner));
        if(status!=XR_XIR_CALL_READY)return status;
        frame->process->published=true;
    }
    if(frame->consume&&frame->process)frame->process->consumed=true;
    return XR_XIR_CALL_READY;
}
static XrXirAction instance_process_resume(XrXirCallView *view,XrXirInstance *instance,uint32_t id,const XrXirCallEntry *entry) {
    XrXirInstance *root=instance_root(instance);XrXirValue *output=view->state;
    if(!value_unit(*output))return instance_pipe_fault(XR_XIR_CALL_BAD_STATE);
    XirProcessFrame *frame=instance_process_frame_find(root,view);XrXirCallStatus status=XR_XIR_CALL_READY;
    if(!frame)status=instance_process_frame_new(view,instance,id,&frame);
    else if(frame->generation!=instance||frame->generation_id!=instance->generation_id||frame->id!=id||
        frame->entry!=xr_xir_call_current_entry(view->activation))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_process_site(view,instance,&frame->origin,view->arguments,view->argument_count);
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    if(id==XR_NATIVE_CALLABLE_SYS_PROCESS_SPAWN)status=instance_process_spawn(view,instance,frame);
    else {
        XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};
        XirProcessOwner *node=instance_process_lookup(instance,view->arguments[0].payload,&current);status=current.status;
        if(node) {
            if(node->references==UINT64_MAX)return instance_pipe_fault(XR_XIR_CALL_LIMIT);
            frame->process=node;++node->references;
        }
        if(status==XR_XIR_CALL_READY&&id==XR_NATIVE_CALLABLE_SYS_PROCESS_TRY_WAIT) {
            XrOsProcessProgress progress={.exited=true,.code=-1};
            if(node)status=instance_pipe_status(xr_os_io_process_poll(&node->owner,&progress));
            XrXirValue code={XR_XIR_I64,0,progress.code};
            if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_nullable_new(entry->result,
                progress.exited?&code:NULL,xr_xir_call_admission(view),output));
            frame->consume=node&&progress.exited;
        } else if(status==XR_XIR_CALL_READY) {
            bool sent=false;if(node)status=instance_pipe_status(xr_os_io_process_kill(&node->owner,view->arguments[1].payload,&sent));
            if(status==XR_XIR_CALL_READY)*output=(XrXirValue){XR_XIR_BOOL,0,sent?1:0};
        }
    }
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    XrXirAction action={0};status=xir_call_return_move_commit(view,output,&action,instance_process_commit,frame);
    return status==XR_XIR_CALL_READY?action:instance_pipe_fault(status);
}
