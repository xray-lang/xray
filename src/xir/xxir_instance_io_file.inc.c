/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io_file.inc.c - Canonical file calls with owned operation results
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
        entry->resume==xr_xir_instance_native_callable && entry->release==xr_xir_instance_native_callable_release &&
        entry->state_bytes==sizeof(XrXirValue) && !entry->environment && !entry->flags && !entry->cleanup_owner &&
        xir_io_file_signature(id,program->types,entry->parameters,entry->parameter_count,entry->result,XR_XIR_NEVER);
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
        count!=entry->parameter_count || (count && !arguments) ||
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
static XrXirCallStatus instance_file_frame_new(XrXirCallView *view,XrXirInstance *instance,uint32_t id,XirFileFrame **output) {
    XirCallSignalOrigin origin={0};XrXirCallStatus status=xir_io_native_can_suspend(id)?
        xir_call_signal_origin(view,&origin):xir_call_private_native_origin(view,&origin);
    uint32_t entry=xr_xir_call_current_entry(view->activation);
    if(status!=XR_XIR_CALL_READY)return status;
    if(origin.target!=entry || !instance->generation_id)return XR_XIR_CALL_BAD_STATE;
    status=instance_io_file_site(view,instance,&origin,view->arguments,view->argument_count);
    if(status!=XR_XIR_CALL_READY)return status;
    XrXirInstance *root=instance_root(instance);XirFileMetadataOwner *owner=NULL;
    bool opening=id==XR_NATIVE_CALLABLE_IO_FILE_OPEN || id==XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE;
    if(!opening)status=instance_file_lookup(view,instance,view->arguments[0].payload,&owner);
    if(status!=XR_XIR_CALL_READY)return status;
    bool valid=owner!=NULL,created=false;int standard=-1;
    if(!opening && view->arguments[0].payload>=0 && view->arguments[0].payload<=2)standard=(int)view->arguments[0].payload;
    if(!owner) {
        owner=instance_metadata_allocate(root->domain,sizeof(*owner),&status);if(!owner)return status;
        owner->kind=XIR_OWNED_FILE_OWNER;owner->file_generation=instance->generation_id;
        owner->next=root->file_metadata_owners;root->file_metadata_owners=owner;created=true;
    }
    XirFileFrame *frame=instance_metadata_allocate(root->domain,sizeof(*frame),&status);
    if(!frame){if(created)instance_file_metadata_owner_unlink(root,owner);return status;}
    *frame=(XirFileFrame){.next=owner->file_frames,.owner=owner,.call=view->activation,.frame=view->state,
        .generation=instance,.generation_id=instance->generation_id,.entry=entry,.id=id,.origin=origin,
        .standard_stream=standard,.valid_handle=valid || standard>=0};
    owner->file_frames=frame;*output=frame;return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_file_open_begin(XrXirCallView *view,XrXirInstance *instance,
    XirFileFrame *frame,const XrXirCallEntry *entry) {
    uint32_t fields[2]={0};XrXirCallStatus status=instance_io_permission(view,instance,frame->entry,fields);
    XrXirValue base={0},text={0};const char *bytes=NULL;size_t length=0;
    if(status==XR_XIR_CALL_READY && !xr_xir_value_from_payload(view->arena,entry->parameters[0],view->arguments[0].payload,&base))
        status=XR_XIR_CALL_BAD_ARGUMENT;
    if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_class_get(&base,fields[0],xr_xir_call_admission(view),&text));
    if(status==XR_XIR_CALL_READY && !xr_xir_string_view(&text,&bytes,&length))status=XR_XIR_CALL_HOST_ERROR;
    frame->begun=true;
    if(status==XR_XIR_CALL_READY && memchr(bytes,0,length))frame->terminal=true;
    if(status==XR_XIR_CALL_READY && !frame->terminal) {
        int64_t token=0;
        do{status=instance_owned_tokens(1,&token);}while(status==XR_XIR_CALL_READY && token<3);
        frame->owner->file_token=(uint64_t)token;
        XirEnvironmentQuery current={view,instance->domain,status};
        if(instance_system_current(&current)) {
            XrOsIoPolicy memory=instance_temp_policy(instance_root(instance));XrOsFileQuery query={&current,instance_system_current};
            XrOsFileRequest request={.action=frame->id==XR_NATIVE_CALLABLE_IO_FILE_OPEN?XR_OS_FILE_OPEN_READ:XR_OS_FILE_OPEN_WRITE,
                .path=bytes,.path_length=length,.append=frame->id==XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE && view->arguments[1].payload!=0,
                .standard_stream=-1};
            XrOsIoStatus started=xr_os_io_file_open_begin(&memory,&query,&request,&frame->owner->file,&frame->operation);
            if(current.status==XR_XIR_CALL_READY)current.status=instance_file_status(started);
        }
        if(current.status==XR_XIR_CALL_READY)instance_system_current(&current);status=current.status;
    }
    xr_xir_value_drop(&text);return status;
}
static XrXirCallStatus instance_file_write_begin(XrXirCallView *view,XrXirInstance *instance,XirFileFrame *frame) {
    bool string=frame->id==XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR;const char *text=NULL;size_t length=0;
    int64_t count=0,offset=view->arguments[2].payload;
    XrXirCallStatus status=XR_XIR_CALL_READY;
    if(string){if(!xr_xir_string_view(view->arguments+1,&text,&length))status=XR_XIR_CALL_BAD_STATE;}
    else {status=value_call_status(xr_xir_array_len(view->arguments+1,xr_xir_call_admission(view),&count));
        if(status==XR_XIR_CALL_READY && (count<0 || (uint64_t)count>SIZE_MAX))status=XR_XIR_CALL_LIMIT;length=(size_t)count;}
    if(status!=XR_XIR_CALL_READY)return status;
    if(offset<0 || (uint64_t)offset>length){frame->terminal=true;frame->begun=true;return XR_XIR_CALL_READY;}
    size_t amount=length-(size_t)offset;if(amount>UINT32_MAX)amount=UINT32_MAX;
    XrXirValueStatus allocated=XR_XIR_VALUE_OK;
    uint8_t *snapshot=amount?xr_xir_domain_call_allocate(instance->domain,amount,&allocated):NULL;
    if(amount && !snapshot)return value_call_status(allocated);
    for(size_t n=0;n<amount && status==XR_XIR_CALL_READY;++n) {
        if(!(n&255u))status=xr_xir_call_execution_status(view);
        if(status!=XR_XIR_CALL_READY)break;
        if(string)snapshot[n]=(uint8_t)text[(size_t)offset+n];
        else {
            XrXirValue byte={0};XrXirFaultDetail fault={0};
            status=value_call_status(xr_xir_array_get(view->arguments+1,offset+(int64_t)n,xr_xir_call_admission(view),&byte,&fault));
            if(status==XR_XIR_CALL_READY && (byte.type!=XR_XIR_U8 || byte.reserved || byte.payload<0 || byte.payload>255))status=XR_XIR_CALL_BAD_STATE;
            if(status==XR_XIR_CALL_READY)snapshot[n]=(uint8_t)byte.payload;xr_xir_value_drop(&byte);
        }
    }
    XirEnvironmentQuery current={view,instance->domain,status};
    if(instance_system_current(&current)) {
        XrOsIoPolicy memory=instance_temp_policy(instance_root(instance));XrOsFileQuery query={&current,instance_system_current};
        XrOsFileRequest request={.action=XR_OS_FILE_WRITE,.standard_stream=frame->standard_stream,.bytes=snapshot,.length=amount};
        frame->begun=true;XrOsIoStatus started=xr_os_io_file_begin(&memory,&query,&request,
            frame->standard_stream<0?&frame->owner->file:NULL,&frame->operation);
        if(current.status==XR_XIR_CALL_READY)current.status=instance_file_status(started);
    }
    if(snapshot)xr_xir_domain_call_deallocate(instance->domain,snapshot,amount);
    if(current.status==XR_XIR_CALL_READY)instance_system_current(&current);return current.status;
}
static XrXirCallStatus instance_file_bytes_value(XrXirCallView *view,XrXirInstance *instance,
    XirFileFrame *frame,XrXirType type,XrXirValue *output) {
    const XrOsFileProgress *read=&frame->observed;size_t count=read->count;
    if(!read->success)return value_call_status(xr_xir_nullable_new(type,NULL,xr_xir_call_admission(view),output));
    if(count && !read->bytes)return XR_XIR_CALL_HOST_ERROR;
    if(count>SIZE_MAX/sizeof(XrXirValue))return XR_XIR_CALL_LIMIT;
    size_t bytes=count*sizeof(XrXirValue);XrXirValueStatus allocated=XR_XIR_VALUE_OK;
    XrXirValue *values=count?xr_xir_domain_call_allocate(instance->domain,bytes,&allocated):NULL;
    if(count && !values)return value_call_status(allocated);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(size_t i=0;i<count;++i) {
        if(!(i&255u)){status=xr_xir_call_execution_status(view);if(status!=XR_XIR_CALL_READY)break;}
        values[i]=(XrXirValue){XR_XIR_U8,0,read->bytes[i]};
    }
    XrXirValue array={0};
    if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_array_new(
        xr_xir_nullable_element(instance->program->types,type),values,count,xr_xir_call_admission(view),&array));
    if(values)xr_xir_domain_call_deallocate(instance->domain,values,bytes);
    if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_nullable_new(type,&array,xr_xir_call_admission(view),output));
    xr_xir_value_drop(&array);return status;
}
static XrXirCallStatus instance_file_io(XrXirCallView *view,XrXirInstance *instance,XirFileFrame *frame,
    const XrXirCallEntry *entry,XrXirValue *output) {
    uint32_t id=frame->id;bool reading=id==XR_NATIVE_CALLABLE_IO_FILE_READ;
    if(!frame->begun) {
        if(!frame->valid_handle || (reading && view->arguments[1].payload<0)) {frame->terminal=true;frame->begun=true;}
        else if(id==XR_NATIVE_CALLABLE_IO_FILE_WRITE || id==XR_NATIVE_CALLABLE_IO_FILE_WRITE_STR) {
            XrXirCallStatus status=instance_file_write_begin(view,instance,frame);if(status!=XR_XIR_CALL_READY)return status;
        }else {
            if(reading && (uint64_t)view->arguments[1].payload>SIZE_MAX)return XR_XIR_CALL_LIMIT;
            XirEnvironmentQuery current={view,instance->domain,XR_XIR_CALL_READY};
            if(instance_system_current(&current)) {
                XrOsIoPolicy memory=instance_temp_policy(instance_root(instance));XrOsFileQuery query={&current,instance_system_current};
                XrOsFileRequest request={.action=reading?XR_OS_FILE_READ:XR_OS_FILE_FLUSH,.standard_stream=frame->standard_stream,
                    .length=reading?(size_t)view->arguments[1].payload:0};
                frame->begun=true;XrOsIoStatus started=xr_os_io_file_begin(&memory,&query,&request,
                    frame->standard_stream<0?&frame->owner->file:NULL,&frame->operation);
                if(current.status==XR_XIR_CALL_READY)current.status=instance_file_status(started);
            }
            if(current.status==XR_XIR_CALL_READY)instance_system_current(&current);
            if(current.status!=XR_XIR_CALL_READY)return current.status;
        }
    }
    if(!frame->terminal) {
        XrOsFileProgress progress={0};XrOsIoStatus polled=xr_os_io_file_poll(&frame->operation,&progress);
        XrXirCallStatus status=instance_file_status(polled);if(status!=XR_XIR_CALL_READY)return status;
        if(progress.terminal) {
            frame->observed=progress;frame->terminal=true;
            status=instance_file_status(progress.status);if(status!=XR_XIR_CALL_READY)return status;
        }
        status=xr_xir_call_execution_status(view);if(status!=XR_XIR_CALL_READY)return status;
        if(!frame->terminal)return XR_XIR_CALL_SUSPENDED;
    }
    XrXirCallStatus status=instance_file_status(frame->observed.status);if(status!=XR_XIR_CALL_READY)return status;
    if(reading)return instance_file_bytes_value(view,instance,frame,entry->result,output);
    if(id==XR_NATIVE_CALLABLE_IO_FILE_FLUSH)*output=(XrXirValue){XR_XIR_BOOL,0,frame->observed.success};
    else if(frame->observed.count>(uint64_t)INT64_MAX)return XR_XIR_CALL_HOST_ERROR;
    else *output=(XrXirValue){XR_XIR_I64,0,frame->observed.success?(int64_t)frame->observed.count:-1};
    return XR_XIR_CALL_READY;
}
static XrXirAction instance_io_file_resume(XrXirCallView *view,XrXirInstance *instance,uint32_t id,const XrXirCallEntry *entry) {
    XrXirValue *output=view->state;XrXirInstance *root=instance_root(instance);
    XirFileFrame *frame=instance_file_frame_find(root,view);XrXirCallStatus status=XR_XIR_CALL_READY;
    if(!value_unit(*output))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY && !frame)status=instance_file_frame_new(view,instance,id,&frame);
    else if(status==XR_XIR_CALL_READY && (frame->generation!=instance || frame->generation_id!=instance->generation_id ||
        frame->entry!=xr_xir_call_current_entry(view->activation) || frame->id!=id))status=XR_XIR_CALL_BAD_STATE;
    if(status==XR_XIR_CALL_READY)status=instance_io_file_site(view,instance,&frame->origin,view->arguments,view->argument_count);
    bool opening=id==XR_NATIVE_CALLABLE_IO_FILE_OPEN || id==XR_NATIVE_CALLABLE_IO_FILE_OPEN_WRITE;
    if(status==XR_XIR_CALL_READY && opening) {
        if(!frame->begun)status=instance_file_open_begin(view,instance,frame,entry);
        if(status==XR_XIR_CALL_READY && !frame->terminal) {
            XrOsFileProgress progress={0};status=instance_file_status(xr_os_io_file_poll(&frame->operation,&progress));
            if(status==XR_XIR_CALL_READY && !progress.terminal)status=XR_XIR_CALL_SUSPENDED;
            else if(status==XR_XIR_CALL_READY){frame->observed=progress;frame->terminal=true;}
        }
        if(status==XR_XIR_CALL_READY)status=instance_file_status(frame->observed.status);
        if(status==XR_XIR_CALL_READY && frame->observed.success && !frame->owner->file.state)status=XR_XIR_CALL_HOST_ERROR;
        if(status==XR_XIR_CALL_READY)*output=(XrXirValue){XR_XIR_I64,0,frame->observed.success?(int64_t)frame->owner->file_token:-1};
    }else if(status==XR_XIR_CALL_READY && id==XR_NATIVE_CALLABLE_IO_FILE_CLOSE) {
        bool closed=false;
        if(frame->valid_handle && frame->standard_stream<0) {
            status=instance_file_status(xr_os_io_file_close(&frame->owner->file,&closed));
            if(status==XR_XIR_CALL_READY && closed)frame->owner->file_token=0;
        }
        if(status==XR_XIR_CALL_READY)*output=(XrXirValue){XR_XIR_BOOL,0,closed};frame->terminal=true;
    }else if(status==XR_XIR_CALL_READY)status=instance_file_io(view,instance,frame,entry,output);
    if(status==XR_XIR_CALL_SUSPENDED)return (XrXirAction){.kind=XR_XIR_ACTION_SUSPEND};
    XrXirAction action={0};
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_return_move(view,output,&action);
    if(status==XR_XIR_CALL_READY){if(opening && frame->observed.success)frame->owner->file_published=true;return action;}
    xr_xir_value_drop(output);return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,status}};
}
