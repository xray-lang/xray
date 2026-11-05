/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io_file_owner.inc.c - Root file tokens and physical frame retirement
 */
struct XirFileFrame {
    XirFileFrame *next;
    XirFileMetadataOwner *owner;
    XrOsFileOperation operation;
    XrXirCall *call;
    XrXirValue *frame;
    XrXirInstance *generation;
    uint64_t generation_id;
    uint32_t entry,id;
    XirCallSignalOrigin origin;
    XrOsFileProgress observed;
    XrXirCallStatus retirement_failure;
    int standard_stream;
    bool begun,terminal,retiring,valid_handle;
};
static XrXirCallStatus instance_file_status(XrOsIoStatus status) {
    switch(status) {
    case XR_OS_IO_OK:return XR_XIR_CALL_READY;
    case XR_OS_IO_OUT_OF_MEMORY:return XR_XIR_CALL_OOM;
    case XR_OS_IO_MEMORY_LIMIT:case XR_OS_IO_OVERFLOW:return XR_XIR_CALL_LIMIT;
    case XR_OS_IO_CANCELLED:return XR_XIR_CALL_CANCELLED;
    case XR_OS_IO_UNSUPPORTED:return XR_XIR_CALL_UNSUPPORTED;
    default:return XR_XIR_CALL_HOST_ERROR;
    }
}
static XrOsIoStatus instance_file_owner_close(XirFileMetadataOwner *owner,bool *closed) {
    if(owner->file_frames){*closed=false;return XR_OS_IO_OK;}
    if(!owner->file.state){*closed=true;return XR_OS_IO_OK;}
    return xr_os_io_file_close(&owner->file,closed);
}
static XirFileFrame *instance_file_frame_find(XrXirInstance *root,const XrXirCallView *view) {
    for(XirFileMetadataOwner *owner=root->file_metadata_owners;owner;owner=owner->next)
        if(owner->kind==XIR_OWNED_FILE_OWNER)
            for(XirFileFrame *frame=owner->file_frames;frame;frame=frame->next)
                if(frame->call==view->activation && frame->frame==view->state)return frame;
    return NULL;
}
static XrXirCallStatus instance_file_lookup(XrXirCallView *view,XrXirInstance *instance,
    int64_t token,XirFileMetadataOwner **output) {
    if(token<3)return XR_XIR_CALL_READY;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    for(XirFileMetadataOwner *owner=instance_root(instance)->file_metadata_owners;owner;owner=owner->next) {
        if(!admission->work || !xr_xir_domain_work(instance->domain,1))return XR_XIR_CALL_LIMIT;
        --admission->work;
        if(owner->kind==XIR_OWNED_FILE_OWNER && owner->file_published && owner->file.state &&
            owner->file_token==(uint64_t)token && owner->file_generation==instance->generation_id) {
            *output=owner;break;
        }
    }
    return xr_xir_call_execution_status(view);
}
XR_FUNC bool xir_instance_io_file_retirement_pending(const void *instance,const XrXirCall *call,
    const XrXirValueAdmission *admission) {
    if(!instance || !admission || admission->function!=admit_function_binding)return false;
    XrXirInstance *root=instance_root((XrXirInstance *)instance);
    for(XirFileMetadataOwner *owner=root->file_metadata_owners;owner;owner=owner->next)
        if(owner->kind==XIR_OWNED_FILE_OWNER)
            for(XirFileFrame *frame=owner->file_frames;frame;frame=frame->next)
                if(frame->call==call && frame->retiring && (frame->operation.state ||
                    (!owner->file_published && owner->file.state)))return true;
    return false;
}
XR_FUNC bool xir_instance_io_file_retire(XrXirCallView *view,XrXirCallStatus *failure) {
    uint32_t entry=UINT32_MAX;const XrXirValueAdmission *admission=xir_call_retirement_admission(view,&entry);
    if(!admission || admission->function!=admit_function_binding || !view->instance)return true;
    XrXirInstance *root=instance_root(view->instance);XirFileFrame *frame=instance_file_frame_find(root,view);
    if(!frame)return true;
    XR_CHECK(entry==frame->entry && frame->generation && admission->context==frame->generation &&
        frame->generation_id==frame->generation->generation_id && frame->generation->domain==root->domain &&
        frame->generation->program->arena==view->arena,"file retirement borrows its exact owned frame and generation");
    frame->retiring=true;bool closed=!frame->operation.state;
    XrOsIoStatus status=closed?XR_OS_IO_OK:xr_os_io_file_operation_close(&frame->operation,&closed);
    if(status==XR_OS_IO_OK && closed && !frame->owner->file_published && frame->owner->file.state)
        status=xr_os_io_file_close(&frame->owner->file,&closed);
    if(status!=XR_OS_IO_OK && frame->retirement_failure==XR_XIR_CALL_READY)
        frame->retirement_failure=instance_file_status(status);
    if(status!=XR_OS_IO_OK || !closed)return false;
    *failure=frame->retirement_failure;return true;
}
static void instance_file_frame_release(XrXirCallView *view) {
    if(!view || !view->instance)return;
    XrXirInstance *root=instance_root(view->instance);XirFileFrame *frame=instance_file_frame_find(root,view);
    if(!frame)return;
    XR_CHECK(frame->generation && frame->generation->domain==root->domain &&
        frame->generation_id==frame->generation->generation_id && !frame->operation.state,
        "a file frame retires only after its worker has joined and closed");
    XirFileMetadataOwner *owner=frame->owner;XirFileFrame **link=&owner->file_frames;
    while(*link && *link!=frame)link=&(*link)->next;
    XR_CHECK(*link==frame,"file release retains the actual Root operation node");*link=frame->next;
    xr_xir_domain_metadata_deallocate(root->domain,frame,sizeof(*frame));
    if(!owner->file_frames && !owner->file.state)instance_file_metadata_owner_unlink(root,owner);
}
