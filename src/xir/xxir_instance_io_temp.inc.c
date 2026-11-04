/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io_temp.inc.c - Prepared values before temporary creation
 *
 * KEY CONCEPT:
 *   The native frame owns every candidate before creation. Return admission
 *   precedes the filesystem commitment and its no-fail staged ownership move.
 */
typedef struct XirIoTempQuery {
    XirEnvironmentQuery work;
    XrXirType result;
    XrXirValue *value;
} XirIoTempQuery;
typedef struct XirIoTempCommit {
    XrXirInstance *root;
    XirFileMetadataOwner **owner;
} XirIoTempCommit;

static XrOsIoStatus instance_temp_status(XrXirCallStatus status) {
    switch(status) {
    case XR_XIR_CALL_READY:return XR_OS_IO_OK;
    case XR_XIR_CALL_OOM:return XR_OS_IO_OUT_OF_MEMORY;
    case XR_XIR_CALL_LIMIT:return XR_OS_IO_MEMORY_LIMIT;
    case XR_XIR_CALL_CANCELLED:return XR_OS_IO_CANCELLED;
    case XR_XIR_CALL_UNSUPPORTED:return XR_OS_IO_UNSUPPORTED;
    default:return XR_OS_IO_IO;
    }
}
static bool instance_temp_current(void *context) {
    XirIoTempQuery *query=context;return instance_system_current(&query->work);
}
static XrOsIoStatus instance_temp_prepare(void *context,const char *candidate,size_t length) {
    XirIoTempQuery *query=context;XrXirCallView *view=query->work.view;
    xr_xir_value_drop(query->value);
    if(!instance_temp_current(query))return instance_temp_status(query->work.status);
    XrXirValue text={0};
    XrXirValueStatus copied=xr_xir_string_new(query->work.domain,candidate,length,&text);
    query->work.status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    if(query->work.status==XR_XIR_CALL_READY)
        query->work.status=value_call_status(xr_xir_nullable_new(query->result,&text,xr_xir_call_admission(view),query->value));
    xr_xir_value_drop(&text);
    if(query->work.status==XR_XIR_CALL_READY && !xr_xir_value_argument(query->value,view->arena,query->result))
        query->work.status=XR_XIR_CALL_BAD_STATE;
    if(query->work.status==XR_XIR_CALL_READY)
        query->work.status=value_call_status(xr_xir_value_admit(query->value,query->result,xr_xir_call_admission(view)));
    if(query->work.status==XR_XIR_CALL_READY)instance_temp_current(query);
    return instance_temp_status(query->work.status);
}
static XrXirCallStatus instance_temp_commit(void *context) {
    XirIoTempCommit *commit=context;XirFileMetadataOwner *owner=*commit->owner;
    XrOsIoPolicy policy=instance_temp_policy(commit->root);
    XrXirCallStatus status=instance_environment_status(xr_os_io_temp_finish(&policy,&owner->temporary,true));
    if(status==XR_XIR_CALL_READY) {
        instance_file_metadata_owner_unlink(commit->root,owner);*commit->owner=NULL;
    }
    return status;
}
static XrXirAction instance_io_temp_action(XrXirCallView *view,XrXirInstance *instance,
    uint32_t index,uint32_t id,const XrXirCallEntry *entry,XrXirValue *value) {
    if(id!=XR_NATIVE_CALLABLE_IO_MAKE_TEMP_DIR && id!=XR_NATIVE_CALLABLE_IO_MAKE_TEMP_FILE)
        return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,XR_XIR_CALL_BAD_STATE}};
    uint32_t unused[2]={0};XrXirCallStatus status=instance_io_permission(view,instance,index,unused);
    XrXirInstance *root=instance_root(instance);XirFileMetadataOwner *owner=NULL;
    XirIoTempQuery query={{view,instance->domain,status},entry->result,value};
    const char *text=NULL;size_t length=0;
    if(status==XR_XIR_CALL_READY && (!xir_io_nullable_string(instance->program->types,entry->result) ||
        !xr_xir_string_view(view->arguments,&text,&length)))query.work.status=XR_XIR_CALL_BAD_ARGUMENT;
    if(query.work.status==XR_XIR_CALL_READY && (length>INT_MAX || !xr_xir_domain_work(instance->domain,(uint64_t)length+1)))
        query.work.status=XR_XIR_CALL_LIMIT;
    if(query.work.status==XR_XIR_CALL_READY && instance_temp_current(&query)) {
        if(!xr_xir_domain_work(root->domain,sizeof(*owner)))query.work.status=XR_XIR_CALL_LIMIT;
        else owner=instance_metadata_allocate(root->domain,sizeof(*owner),&query.work.status);
        if(owner) {
            owner->kind=XIR_FILE_TEMP_OWNER;owner->next=root->file_metadata_owners;root->file_metadata_owners=owner;
        }
    }
    bool created=false;
    if(query.work.status==XR_XIR_CALL_READY) {
        XrOsIoPolicy policy=instance_temp_policy(root);
        XrOsFileTempRequest request={&policy,{text,length},&query,instance_temp_current,instance_temp_prepare};
        XrOsIoStatus called=id==XR_NATIVE_CALLABLE_IO_MAKE_TEMP_DIR?
            xr_os_io_temp_directory(&request,&owner->temporary,&created):xr_os_io_temp_file(&request,&owner->temporary,&created);
        if(query.work.status==XR_XIR_CALL_READY)query.work.status=instance_environment_status(called);
    }
    if(owner && !owner->temporary.state){instance_file_metadata_owner_unlink(root,owner);owner=NULL;}
    if(query.work.status==XR_XIR_CALL_READY && !created && owner)query.work.status=XR_XIR_CALL_BAD_STATE;
    XrXirAction action={0};
    if(query.work.status==XR_XIR_CALL_READY && instance_temp_current(&query)) {
        if(created) {
            if(!owner || !value->type)query.work.status=XR_XIR_CALL_BAD_STATE;
            else {
                XirIoTempCommit commit={root,&owner};
                query.work.status=xir_call_return_move_commit(view,value,&action,instance_temp_commit,&commit);
            }
        } else {
            xr_xir_value_drop(value);
            query.work.status=value_call_status(xr_xir_nullable_new(entry->result,NULL,xr_xir_call_admission(view),value));
            if(query.work.status==XR_XIR_CALL_READY)query.work.status=xr_xir_call_return_move(view,value,&action);
        }
    }
    if(query.work.status==XR_XIR_CALL_READY)return action;
    xr_xir_value_drop(value);
    return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,query.work.status}};
}
