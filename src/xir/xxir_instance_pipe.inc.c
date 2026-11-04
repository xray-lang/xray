/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_pipe.inc.c - Authenticated pipe calls with resumable owned I/O
 */
static bool instance_pipe_target(const XrXirProgram *program,uint32_t target) {
    if(!program || target>=program->entry_count || !program->declarations ||
        !program->declarations->functions || !program->permissions || !program->permissions->input ||
        !program->permissions->input->artifact || !program->permissions->effects || !program->permissions->entries ||
        program->permissions->function_count!=program->entry_count ||
        !xir_sys_native_scope(program->declarations,target,program->entry_count))return false;
    uint32_t id=program->declarations->functions[target].native_callable;
    const XrXirCallEntry *entry=program->entries+target;
    XrXirEffectFacts facts=xr_xir_effects_read(program->permissions->effects,target);
    return xir_sys_pipe_id(id) && xir_native_callable_identity(program->declarations->functions+target) &&
        facts.status==XR_XIR_OK && facts.control.suspend==(xir_sys_pipe_can_suspend(id)?XR_XIR_EFFECT_MAY:XR_XIR_EFFECT_NONE) &&
        entry->resume==xr_xir_instance_native_callable && entry->release==xr_xir_instance_native_callable_release &&
        entry->state_bytes==sizeof(XrXirValue) && !entry->environment && !entry->flags && !entry->cleanup_owner &&
        xir_sys_pipe_signature(id,program->types,entry->parameters,entry->parameter_count,entry->result,XR_XIR_NEVER);
}
XR_FUNC bool xir_instance_pipe_nosuspend_transport(XrXirCallView *view,uint32_t target) {
    if(!view || !instance_view_current(view))return false;
    XrXirInstance *instance=view_instance(view);
    if(!instance || !instance_pipe_target(instance->program,target))return false;
    uint32_t id=instance->program->declarations->functions[target].native_callable;
    return id==XR_NATIVE_CALLABLE_SYS_PIPE_OPEN || id==XR_NATIVE_CALLABLE_SYS_PIPE_CLOSE;
}
static XrXirCallStatus instance_pipe_site(XrXirCallView *view,XrXirInstance *instance,
    const XirCallSignalOrigin *origin,const XrXirValue *arguments,uint32_t count) {
    const XrXirProgram *program=instance->program;
    if(!xr_xir_call_admission(view) || view->phase!=XR_XIR_CALL_NORMAL || view->scope_exit ||
        !instance_pipe_target(program,origin->target))return XR_XIR_CALL_BAD_STATE;
    const XrXirArtifact *artifact=program->permissions->input->artifact;
    const XrXirModule *module=&artifact->module;
    if(artifact->program_final!=program->permissions->effects || module->stage!=XR_XIR_LOWERED ||
        module->function_count!=program->entry_count || !module->functions || origin->caller>=module->function_count ||
        origin->instruction>=module->functions[origin->caller].instruction_count)return XR_XIR_CALL_BAD_STATE;
    const XrXirInstruction *op=module->functions[origin->caller].instructions+origin->instruction;
    const XrXirCallEntry *entry=program->entries+origin->target;
    uint32_t id=program->declarations->functions[origin->target].native_callable;
    if(op->op!=XR_XIR_CALL || op->immediate!=(int64_t)origin->target || op->args[1]!=count ||
        count!=entry->parameter_count || (count && !arguments) ||
        (xir_sys_pipe_can_suspend(id) && xr_xir_call_cleanup_active(view->activation)))return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(admission->context!=instance || admission->domain!=instance->domain || admission->arena!=program->arena)
        return XR_XIR_CALL_BAD_STATE;
    for(uint32_t p=0;p<count;++p) {
        XrXirCallStatus status=admit_instance_value(admission,arguments+p,entry->parameters[p]);
        if(status!=XR_XIR_CALL_READY)return status;
    }
    return xr_xir_call_execution_status(view);
}
static XrXirCallStatus instance_pipe_stage(XrXirCallView *view,uint32_t instruction,uint32_t target,
    const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);if(!instance)return XR_XIR_CALL_BAD_STATE;
    XirCallSignalOrigin origin={xr_xir_call_current_entry(view->activation),instruction,target};
    XrXirCallStatus status=instance_pipe_site(view,instance,&origin,arguments,count);
    if(status!=XR_XIR_CALL_READY)return status;
    return xir_sys_pipe_can_suspend(instance->program->declarations->functions[target].native_callable)?
        xir_call_signal_stage(view,instruction,target):xir_call_private_native_stage(view,instruction,target);
}
static XrXirCallStatus instance_pipe_frame_new(XrXirCallView *view,XrXirInstance *instance,
    uint32_t id,XirPipeFrame **output) {
    XirCallSignalOrigin origin={0};
    XrXirCallStatus status=xir_sys_pipe_can_suspend(id)?xir_call_signal_origin(view,&origin):
        xir_call_private_native_origin(view,&origin);
    if(status!=XR_XIR_CALL_READY)return status;
    uint32_t entry=xr_xir_call_current_entry(view->activation);
    if(origin.target!=entry)return XR_XIR_CALL_BAD_STATE;
    status=instance_pipe_site(view,instance,&origin,view->arguments,view->argument_count);
    if(status!=XR_XIR_CALL_READY)return status;
    XrXirInstance *root=instance_root(instance);
    if(!instance_pipe_owners_close(root,false))return XR_XIR_CALL_HOST_ERROR;
    XirPipeFrame *frame=instance_metadata_allocate(root->domain,sizeof(*frame),&status);
    if(!frame)return status;
    *frame=(XirPipeFrame){.call=view->activation,.frame=view->state,.generation=instance,
        .generation_id=instance->generation_id,.entry=entry,.id=id,.origin=origin,.next=root->pipe_frames};
    root->pipe_frames=frame;*output=frame;return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_pipe_open_value(XrXirCallView *view,XrXirInstance *instance,
    XirPipeFrame *frame,XrXirType type,XrXirValue *output) {
    XrXirInstance *root=instance_root(instance);XrXirCallStatus status=XR_XIR_CALL_READY;
    XirPipeOwner *pipe=instance_metadata_allocate(root->domain,sizeof(*pipe),&status);
    if(!pipe)return status;
    *pipe=(XirPipeOwner){.next=root->pipe_owners,.references=1};root->pipe_owners=pipe;frame->pipe=pipe;
    status=instance_pipe_token_pair(pipe->tokens);if(status!=XR_XIR_CALL_READY)return status;
    XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};
    XrOsPipeQuery query={&current,instance_pipe_current};
    XrOsIoPolicy memory={root->domain,instance_pipe_allocate,instance_pipe_free};
    bool opened=false;XrOsIoStatus created=xr_os_io_pipe_open(&memory,&query,&pipe->owner,&opened);
    if(!instance_pipe_current(&current))return current.status;
    status=instance_pipe_status(created);if(status!=XR_XIR_CALL_READY)return status;
    XrXirValue pair={0};
    if(opened) {
        XrXirValue values[]={{XR_XIR_I64,0,pipe->tokens[0]},{XR_XIR_I64,0,pipe->tokens[1]}};
        XrXirType element=xr_xir_nullable_element(instance->program->types,type);
        status=value_call_status(xr_xir_tuple_new(element,values,2,xr_xir_call_admission(view),&pair));
    }
    if(status==XR_XIR_CALL_READY)
        status=value_call_status(xr_xir_nullable_new(type,opened?&pair:NULL,xr_xir_call_admission(view),output));
    xr_xir_value_drop(&pair);frame->terminal=true;frame->opened=opened;return status;
}
static XrXirCallStatus instance_pipe_bytes_value(XrXirCallView *view,XrXirInstance *instance,
    const XrOsPipeProgress *read,XrXirType type,XrXirValue *output) {
    size_t count=read->count;
    if(count && !read->bytes)return XR_XIR_CALL_HOST_ERROR;
    if(count>SIZE_MAX/sizeof(XrXirValue))return XR_XIR_CALL_LIMIT;
    XrXirValueStatus allocated=XR_XIR_VALUE_OK;size_t bytes=count*sizeof(XrXirValue);
    XrXirValue *values=count?xr_xir_domain_call_allocate(instance->domain,bytes,&allocated):NULL;
    if(count && !values)return value_call_status(allocated);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(size_t i=0;i<count;++i) {
        if((i&255u)==0) {
            status=xr_xir_call_execution_status(view);
            size_t charge=count-i;if(charge>256)charge=256;
            if(status==XR_XIR_CALL_READY && !xr_xir_domain_work(instance->domain,charge*sizeof(*values)))status=XR_XIR_CALL_LIMIT;
            if(status!=XR_XIR_CALL_READY)break;
        }
        values[i]=(XrXirValue){XR_XIR_U8,0,read->bytes[i]};
    }
    XrXirValue array={0};
    if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_array_new(
        xr_xir_nullable_element(instance->program->types,type),values,count,xr_xir_call_admission(view),&array));
    if(values)xr_xir_domain_call_deallocate(instance->domain,values,bytes);
    if(status==XR_XIR_CALL_READY)
        status=value_call_status(xr_xir_nullable_new(type,&array,xr_xir_call_admission(view),output));
    xr_xir_value_drop(&array);return status;
}
static XrXirCallStatus instance_pipe_write_begin(XrXirCallView *view,XrXirInstance *instance,XirPipeFrame *frame) {
    XrXirValueAdmission *admission=xr_xir_call_admission(view);int64_t length=0;
    XrXirValueStatus sized=xr_xir_array_len(view->arguments+1,admission,&length);
    if(sized!=XR_XIR_VALUE_OK)return value_call_status(sized);
    if(length<0 || (uint64_t)length>SIZE_MAX)return XR_XIR_CALL_BAD_ARGUMENT;
    size_t count=(size_t)length;if(count>UINT32_MAX)count=UINT32_MAX;
    XrXirValueStatus allocated=XR_XIR_VALUE_OK;
    uint8_t *snapshot=count?xr_xir_domain_call_allocate(instance->domain,count,&allocated):NULL;
    if(count && !snapshot)return value_call_status(allocated);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(size_t i=0;i<count;++i) {
        if(!(i&255u)) {status=xr_xir_call_execution_status(view);if(status!=XR_XIR_CALL_READY)break;}
        XrXirValue byte={0};XrXirFaultDetail fault={0};
        XrXirValueStatus copied=xr_xir_array_get(view->arguments+1,(int64_t)i,admission,&byte,&fault);
        if(copied!=XR_XIR_VALUE_OK) {status=value_call_status(copied);break;}
        if(byte.type!=XR_XIR_U8 || byte.reserved || byte.payload<0 || byte.payload>255) {
            xr_xir_value_drop(&byte);status=XR_XIR_CALL_BAD_STATE;break;
        }
        snapshot[i]=(uint8_t)byte.payload;xr_xir_value_drop(&byte);
    }
    if(status==XR_XIR_CALL_READY) {
        XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};XrOsPipeQuery query={&current,instance_pipe_current};
        XrOsPipeRequest request={XR_OS_PIPE_WRITE,snapshot,count};frame->begun=true;
        status=instance_pipe_status(xr_os_io_pipe_begin(&frame->pipe->owner,&request,&query,&frame->operation));
        if(!instance_pipe_current(&current))status=current.status;
    }
    if(snapshot)xr_xir_domain_call_deallocate(instance->domain,snapshot,count);
    return status;
}
static XrXirCallStatus instance_pipe_io(XrXirCallView *view,XrXirInstance *instance,
    XirPipeFrame *frame,uint32_t id,XrXirType type,XrXirValue *output) {
    XrXirInstance *root=instance_root(instance);bool reading=id==XR_NATIVE_CALLABLE_SYS_PIPE_READ;
    if(!frame->begun) {
        XirPipeQuery current={view,instance->domain,XR_XIR_CALL_READY};uint32_t end=0;
        XirPipeOwner *pipe=instance_pipe_lookup(root,view->arguments[0].payload,&end,&current);
        if(current.status!=XR_XIR_CALL_READY)return current.status;
        bool invalid=!pipe || end!=(uint32_t)(reading?XR_OS_PIPE_READ:XR_OS_PIPE_WRITE) || (reading && view->arguments[1].payload<0);
        if(invalid) {
            frame->terminal=true;
            if(!reading){*output=(XrXirValue){XR_XIR_I64,0,-1};return XR_XIR_CALL_READY;}
            return value_call_status(xr_xir_nullable_new(type,NULL,xr_xir_call_admission(view),output));
        }
        if(pipe->references==UINT64_MAX)return XR_XIR_CALL_LIMIT;
        frame->pipe=pipe;++pipe->references;
        XrXirCallStatus status=XR_XIR_CALL_READY;
        if(reading) {
            if((uint64_t)view->arguments[1].payload>SIZE_MAX)return XR_XIR_CALL_LIMIT;
            XrOsPipeRequest request={XR_OS_PIPE_READ,NULL,(size_t)view->arguments[1].payload};
            XrOsPipeQuery query={&current,instance_pipe_current};frame->begun=true;
            status=instance_pipe_status(xr_os_io_pipe_begin(&pipe->owner,&request,&query,&frame->operation));
            if(!instance_pipe_current(&current))status=current.status;
        } else status=instance_pipe_write_begin(view,instance,frame);
        if(status!=XR_XIR_CALL_READY)return status;
    }
    if(!frame->terminal) {
        XrOsPipeProgress progress={0};XrOsIoStatus polled=xr_os_io_pipe_poll(&frame->operation,&progress);
        XrXirCallStatus status=xr_xir_call_execution_status(view);
        if(status!=XR_XIR_CALL_READY)return status;
        if(polled!=XR_OS_IO_OK)return instance_pipe_status(polled);
        if(!progress.terminal)return XR_XIR_CALL_SUSPENDED;
        frame->observed=progress;frame->terminal=true;
    }
    if(!reading && frame->observed.status==XR_OS_IO_END){*output=(XrXirValue){XR_XIR_I64,0,-1};return XR_XIR_CALL_READY;}
    XrXirCallStatus status=instance_pipe_status(frame->observed.status);
    if(status!=XR_XIR_CALL_READY)return status;
    if(reading)return instance_pipe_bytes_value(view,instance,&frame->observed,type,output);
    if(frame->observed.count>(uint64_t)INT64_MAX)return XR_XIR_CALL_HOST_ERROR;
    *output=(XrXirValue){XR_XIR_I64,0,(int64_t)frame->observed.count};return XR_XIR_CALL_READY;
}
static XrXirAction instance_pipe_fault(XrXirCallStatus status) {
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,status}};
}
static XrXirAction instance_pipe_resume(XrXirCallView *view,XrXirInstance *instance,uint32_t id,const XrXirCallEntry *entry) {
    XrXirInstance *root=instance_root(instance);XrXirValue *output=view->state;
    XirPipeFrame *frame=instance_pipe_frame_find(root,view);XrXirCallStatus status=XR_XIR_CALL_READY;
    if(!value_unit(*output))return instance_pipe_fault(XR_XIR_CALL_BAD_STATE);
    if(!frame)status=instance_pipe_frame_new(view,instance,id,&frame);
    else if(frame->generation!=instance || frame->generation_id!=instance->generation_id || frame->entry!=
        xr_xir_call_current_entry(view->activation) || frame->id!=id)status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_pipe_site(view,instance,&frame->origin,view->arguments,view->argument_count);
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    if(id==XR_NATIVE_CALLABLE_SYS_PIPE_OPEN)status=instance_pipe_open_value(view,instance,frame,entry->result,output);
    else if(id==XR_NATIVE_CALLABLE_SYS_PIPE_CLOSE) {
        bool closed=false;
        status=instance_pipe_close_endpoint(view,instance,view->arguments[0].payload,UINT32_MAX,&frame->pipe,&closed);
        if(status==XR_XIR_CALL_READY)*output=(XrXirValue){XR_XIR_BOOL,0,closed?1:0};
        frame->terminal=true;
    } else status=instance_pipe_io(view,instance,frame,id,entry->result,output);
    if(status==XR_XIR_CALL_SUSPENDED)return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    XrXirAction action={0};status=xr_xir_call_return_move(view,output,&action);
    if(status!=XR_XIR_CALL_READY)return instance_pipe_fault(status);
    if(id==XR_NATIVE_CALLABLE_SYS_PIPE_OPEN && frame->pipe && frame->opened)frame->pipe->published=true;
    return action;
}
