/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_process_owner.inc.c - Generation-bound tokens and process retirement
 */
#include "../os/os_process_owned.h"
struct XirProcessOwner {
    XirProcessOwner *next;
    XrOsProcessOwner owner;
    int64_t token;
    uint64_t generation,references;
    bool published,consumed,detached;
};
struct XirProcessFrame {
    XirProcessFrame *next;
    XirProcessOwner *process;
    XrXirCall *call;
    XrXirValue *frame;
    XrXirInstance *generation;
    uint64_t generation_id;
    uint32_t entry,id;
    XirCallSignalOrigin origin;
    XrXirCallStatus retirement_failure;
    bool created,consume,retiring;
};
static XrXirCallStatus instance_process_token(int64_t *output) {return instance_owned_tokens(1,output);}
static XirProcessFrame *instance_process_frame_find(XrXirInstance *root,const XrXirCallView *view) {
    for(XirProcessFrame *frame=root->process_frames;frame;frame=frame->next)
        if(frame->call==view->activation&&frame->frame==view->state)return frame;
    return NULL;
}
static bool instance_process_close_selected(XrXirInstance *root,bool all,uint64_t generation) {
    XR_CHECK(root==instance_root(root)&&root->driving,"process close retains the busy Root");
    bool complete=true;
    XirProcessOwner **link=&root->process_owners;
    while(*link) {
        XirProcessOwner *node=*link;
        if(generation&&node->generation!=generation){link=&node->next;continue;}
        if(!all&&node->published&&!node->detached){link=&node->next;continue;}
        if(node->references){complete=false;link=&node->next;continue;}
        bool closed=!node->owner.state;
        XrOsIoStatus status=closed?XR_OS_IO_OK:xr_os_io_process_close(&node->owner,&closed);
        if(status==XR_OS_IO_OK&&closed) {
            *link=node->next;xr_xir_domain_metadata_deallocate(root->domain,node,sizeof(*node));
        } else {complete=false;link=&node->next;}
    }
    return complete;
}
static bool instance_process_owners_close(XrXirInstance *root,bool all) {
    return instance_process_close_selected(root,all,0);
}
static bool instance_process_generation_close(XrXirInstance *instance) {
    return instance_process_close_selected(instance_root(instance),true,instance->generation_id);
}
static XirProcessOwner *instance_process_lookup(XrXirInstance *instance,int64_t token,XirPipeQuery *query) {
    if(token<=0)return NULL;
    for(XirProcessOwner *node=instance_root(instance)->process_owners;node;node=node->next) {
        if(!instance_pipe_current(query))return NULL;
        if(node->token==token&&node->generation==instance->generation_id&&node->published&&!node->consumed&&!node->detached)
            return node;
    }
    return NULL;
}
XR_FUNC bool xir_instance_process_retirement_pending(const void *instance,const XrXirCall *call,
    const XrXirValueAdmission *admission) {
    if(!instance||!admission||admission->function!=admit_function_binding)return false;
    const XrXirInstance *root=instance_root((XrXirInstance *)instance);
    for(const XirProcessFrame *frame=root->process_frames;frame;frame=frame->next)
        if(frame->call==call&&frame->retiring&&frame->process&&!frame->process->published&&frame->process->owner.state)return true;
    return false;
}
XR_FUNC bool xir_instance_process_retire(XrXirCallView *view,XrXirCallStatus *failure) {
    uint32_t entry=UINT32_MAX;
    const XrXirValueAdmission *admission=xir_call_retirement_admission(view,&entry);
    if(!admission||admission->function!=admit_function_binding||!view->instance)return true;
    XrXirInstance *root=instance_root(view->instance);
    XirProcessFrame *frame=instance_process_frame_find(root,view);if(!frame)return true;
    XR_CHECK(frame->entry==entry&&frame->generation&&admission->context==frame->generation&&
        frame->generation_id==frame->generation->generation_id&&frame->generation->domain==root->domain&&
        frame->generation->program->arena==view->arena,"process retirement retains its actual frame and generation");
    frame->retiring=true;
    if(frame->process&&!frame->process->published&&frame->process->owner.state) {
        bool closed=false;XrOsIoStatus status=xr_os_io_process_close(&frame->process->owner,&closed);
        if(status!=XR_OS_IO_OK&&frame->retirement_failure==XR_XIR_CALL_READY)
            frame->retirement_failure=instance_pipe_status(status);
        if(status!=XR_OS_IO_OK||!closed)return false;
    }
    *failure=frame->retirement_failure;return true;
}
static void instance_process_frame_release(XrXirCallView *view) {
    if(!view||!view->instance)return;
    XrXirInstance *root=instance_root(view->instance);
    XirProcessFrame *frame=instance_process_frame_find(root,view);if(!frame)return;
    XR_CHECK(frame->generation&&frame->generation_id==frame->generation->generation_id&&
        frame->generation->program->arena==view->arena,"process release retains its actual generation");
    XR_CHECK(!frame->process||frame->process->published||!frame->process->owner.state,
        "unpublished process execution terminates before its frame can retire");
    if(frame->process){XR_CHECK(frame->process->references,"process frame owns one reference");--frame->process->references;}
    frame->call=NULL;frame->frame=NULL;frame->generation=NULL;frame->generation_id=0;
    XirProcessFrame **link=&root->process_frames;
    while(*link&&*link!=frame)link=&(*link)->next;
    XR_CHECK(*link==frame,"process release retains its actual Root node");*link=frame->next;
    xr_xir_domain_metadata_deallocate(root->domain,frame,sizeof(*frame));
    bool driving=root->driving;root->driving=true;
    (void)instance_process_owners_close(root,false);root->driving=driving;
}
