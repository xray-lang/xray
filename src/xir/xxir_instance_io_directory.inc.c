/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io_directory.inc.c - Generation-bound cursors and staged names
 *
 * KEY CONCEPT:
 *   Tokens never encode addresses or repeat. A consumed native name remains
 *   root-owned until its fully admitted return commits, without guest replay.
 */
static _Atomic(uint64_t) instance_directory_serial=1;
static XrXirCallStatus instance_directory_token(uint64_t *output) {
    uint64_t next=atomic_load_explicit(&instance_directory_serial,memory_order_relaxed);
    for(;;) {
        if(next>(uint64_t)INT64_MAX)return XR_XIR_CALL_LIMIT;
        if(atomic_compare_exchange_weak_explicit(&instance_directory_serial,&next,next+1,
            memory_order_relaxed,memory_order_relaxed)){*output=next;return XR_XIR_CALL_READY;}
    }
}
static XrXirCallStatus instance_directory_lookup(XrXirCallView *view,XrXirInstance *instance,
    int64_t token,XirFileMetadataOwner **output) {
    if(token<=0 || !instance->generation_id)return XR_XIR_CALL_BAD_ARGUMENT;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    for(XirFileMetadataOwner *node=instance_root(instance)->file_metadata_owners;node;node=node->next) {
        if(!admission->work || !xr_xir_domain_work(instance->domain,1))return XR_XIR_CALL_LIMIT;
        --admission->work;
        if(node->kind==XIR_DIRECTORY_OWNER && node->directory_token==(uint64_t)token && node->directory_published &&
            node->directory_generation==instance->generation_id && node->directory.state){*output=node;return XR_XIR_CALL_READY;}
    }
    return XR_XIR_CALL_BAD_ARGUMENT;
}
typedef struct XirDirectoryCommit {XrXirInstance *root;XirFileMetadataOwner *owner;bool publish;} XirDirectoryCommit;
static XrXirCallStatus instance_directory_commit(void *context) {
    XirDirectoryCommit *commit=context;
    if(commit->publish)commit->owner->directory_published=true;
    else instance_directory_name_drop(commit->root,commit->owner);
    return XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_directory_open(XrXirCallView *view,XrXirInstance *instance,
    XrXirType path,uint32_t field,XirFileMetadataOwner **output,XrXirValue *value) {
    if(!instance->generation_id)return XR_XIR_CALL_BAD_STATE;
    XrXirInstance *root=instance_root(instance);XrXirValue base={0},text={0};
    if(!xr_xir_value_from_payload(view->arena,path,view->arguments[0].payload,&base))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    query.status=value_call_status(xr_xir_class_get(&base,field,xr_xir_call_admission(view),&text));
    const char *bytes=NULL;size_t length=0;
    if(query.status==XR_XIR_CALL_READY && !xr_xir_string_view(&text,&bytes,&length))query.status=XR_XIR_CALL_HOST_ERROR;
    if(query.status==XR_XIR_CALL_READY && (length>INT_MAX || !xr_xir_domain_work(instance->domain,(uint64_t)length+1)))query.status=XR_XIR_CALL_LIMIT;
    XirFileMetadataOwner *owner=NULL;uint64_t token=0;
    if(query.status==XR_XIR_CALL_READY)query.status=instance_directory_token(&token);
    if(query.status==XR_XIR_CALL_READY && instance_system_current(&query)) {
        if(!xr_xir_domain_work(root->domain,sizeof(*owner)))query.status=XR_XIR_CALL_LIMIT;
        else owner=instance_metadata_allocate(root->domain,sizeof(*owner),&query.status);
        if(owner) {
            owner->kind=XIR_DIRECTORY_OWNER;owner->directory_token=token;owner->directory_generation=instance->generation_id;
            owner->next=root->file_metadata_owners;root->file_metadata_owners=owner;
        }
    }
    bool opened=false;
    if(query.status==XR_XIR_CALL_READY) {
        XrOsIoPolicy policy=instance_temp_policy(root);
        XrOsDirectoryRequest request={&policy,{bytes,length},&query,instance_system_current};
        XrOsIoStatus status=xr_os_io_directory_open(&request,&owner->directory,&opened);
        if(query.status==XR_XIR_CALL_READY)query.status=instance_environment_status(status);
    }
    xr_xir_value_drop(&text);
    if(owner && !owner->directory.state){instance_file_metadata_owner_unlink(root,owner);owner=NULL;}
    if(query.status==XR_XIR_CALL_READY && opened!=!!owner)query.status=XR_XIR_CALL_BAD_STATE;
    if(instance_system_current(&query))*value=(XrXirValue){XR_XIR_I64,0,opened?(int64_t)token:0};
    *output=owner;return query.status;
}
static XrXirCallStatus instance_directory_next(XrXirCallView *view,XrXirInstance *instance,
    const XrXirCallEntry *entry,XirFileMetadataOwner *owner,XrXirValue *value) {
    if(owner->directory_closing)return XR_XIR_CALL_BAD_ARGUMENT;
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!owner->pending_name.bytes && !owner->directory_end && instance_system_current(&query)) {
        XrOsDirectoryText name={0};
        XrOsIoStatus status=xr_os_io_directory_next(&owner->directory,&query,instance_system_current,&name);
        /* Transfer the returned owner before any post-call control can fail. */
        if(status==XR_OS_IO_OK) {
            owner->pending_name=name;
            if(!name.bytes || !name.length)query.status=XR_XIR_CALL_HOST_ERROR;
        }else if(name.bytes || name.length) {owner->pending_name=name;query.status=XR_XIR_CALL_HOST_ERROR;}
        else if(status==XR_OS_IO_END)owner->directory_end=true;
        else if(query.status==XR_XIR_CALL_READY)query.status=instance_environment_status(status);
    }
    XrXirValue text={0};
    if(instance_system_current(&query) && owner->pending_name.bytes) {
        XrXirValueStatus copied=xr_xir_string_new(instance->domain,owner->pending_name.bytes,owner->pending_name.length,&text);
        query.status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    }
    if(instance_system_current(&query))query.status=value_call_status(xr_xir_nullable_new(entry->result,
        owner->pending_name.bytes?&text:NULL,xr_xir_call_admission(view),value));
    xr_xir_value_drop(&text);
    if(query.status==XR_XIR_CALL_READY)instance_system_current(&query);
    return query.status;
}
static XrXirAction instance_io_directory_action(XrXirCallView *view,XrXirInstance *instance,
    uint32_t index,uint32_t id,const XrXirCallEntry *entry,XrXirValue *value) {
    if(id<XR_NATIVE_CALLABLE_IO_DIR_OPEN || id>XR_NATIVE_CALLABLE_IO_DIR_CLOSE)
        return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,XR_XIR_CALL_BAD_STATE}};
    uint32_t fields[2]={0};XrXirCallStatus status=instance_io_permission(view,instance,index,fields);
    XrXirInstance *root=instance_root(instance);XirFileMetadataOwner *owner=NULL;
    if(status==XR_XIR_CALL_READY && id==XR_NATIVE_CALLABLE_IO_DIR_OPEN) {
        if(entry->result!=XR_XIR_I64)status=XR_XIR_CALL_BAD_STATE;
        else status=instance_directory_open(view,instance,entry->parameters[0],fields[0],&owner,value);
    }else if(status==XR_XIR_CALL_READY) {
        if(id==XR_NATIVE_CALLABLE_IO_DIR_CLOSE && entry->result!=XR_XIR_BOOL)status=XR_XIR_CALL_BAD_STATE;
        if(status==XR_XIR_CALL_READY && !xr_xir_value_argument(view->arguments,view->arena,XR_XIR_I64))status=XR_XIR_CALL_BAD_ARGUMENT;
        else if(status==XR_XIR_CALL_READY)status=instance_directory_lookup(view,instance,view->arguments[0].payload,&owner);
        if(status==XR_XIR_CALL_READY && id==XR_NATIVE_CALLABLE_IO_DIR_NEXT) {
            if(!xir_io_nullable_string(instance->program->types,entry->result))status=XR_XIR_CALL_BAD_STATE;
            else status=instance_directory_next(view,instance,entry,owner,value);
        }else if(status==XR_XIR_CALL_READY && id==XR_NATIVE_CALLABLE_IO_DIR_CLOSE) {
            owner->directory_closing=true;XrOsIoStatus closed=xr_os_io_directory_close(&owner->directory);
            if(closed==XR_OS_IO_OK) {
                /* Native close is already final, even if later return control
                 * refuses. Remove the token now so no failure re-closes it. */
                instance_directory_name_drop(root,owner);instance_file_metadata_owner_unlink(root,owner);owner=NULL;
                *value=(XrXirValue){XR_XIR_BOOL,0,1};
            }else if(closed==XR_OS_IO_IO)*value=(XrXirValue){XR_XIR_BOOL,0,0};
            else status=instance_environment_status(closed);
        }else if(status==XR_XIR_CALL_READY)status=XR_XIR_CALL_BAD_STATE;
    }
    XrXirAction action={0};
    if(status==XR_XIR_CALL_READY) {
        if(id!=XR_NATIVE_CALLABLE_IO_DIR_CLOSE && owner) {
            XirDirectoryCommit commit={root,owner,id==XR_NATIVE_CALLABLE_IO_DIR_OPEN};
            status=xir_call_return_move_commit(view,value,&action,instance_directory_commit,&commit);
        }else status=xr_xir_call_return_move(view,value,&action);
    }
    if(status==XR_XIR_CALL_READY)return action;
    xr_xir_value_drop(value);return (XrXirAction){.kind=XR_XIR_ACTION_FAULT,.value={XR_XIR_I64,0,status}};
}
