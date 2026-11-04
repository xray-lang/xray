/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_dylib_owner.inc.c - Loader tokens and non-retaining pointer origins
 */
#include "../os/os_dylib_owned.h"
#include "xxir_pointer_origin_internal.h"
struct XirDylibOwner {
    XirDylibOwner *next;
    XrOsDylibOwner owner;
    XirPointerOrigin *origin;
    uint64_t generation,references;
    int64_t token;
    bool published,closing,consumed;
};
struct XirDylibFrame {
    XirDylibFrame *next;
    XirDylibOwner *library;
    XrXirCall *call;
    XrXirValue *frame;
    XrXirInstance *generation;
    uint64_t generation_id;
    uint32_t entry,id;
    XirCallSignalOrigin origin;
    XrXirCallStatus retirement_failure;
    bool opened,closed,retiring;
};
static XrOsIoStatus instance_dylib_close_owner(XirDylibOwner *node,bool *closed) {
    node->closing=true;xir_pointer_origin_invalidate(&node->origin);
    if(!node->owner.state){*closed=true;return XR_OS_IO_OK;}
    return xr_os_io_dylib_close(&node->owner,closed);
}
static bool instance_dylib_close_selected(XrXirInstance *root,bool all,uint64_t generation) {
    XR_CHECK(root==instance_root(root)&&root->driving,"loader close retains its busy Root");
    bool complete=true;XirDylibOwner **link=&root->dylib_owners;
    while(*link) {
        XirDylibOwner *node=*link;
        if(generation&&node->generation!=generation){link=&node->next;continue;}
        if(!all&&node->published&&!node->consumed){link=&node->next;continue;}
        if(node->references){complete=false;link=&node->next;continue;}
        bool closed=false;XrOsIoStatus status=instance_dylib_close_owner(node,&closed);
        if(status==XR_OS_IO_OK&&closed){*link=node->next;xr_xir_domain_metadata_deallocate(root->domain,node,sizeof(*node));}
        else {complete=false;link=&node->next;}
    }
    return complete;
}
static bool instance_dylib_owners_close(XrXirInstance *root,bool all) {
    return instance_dylib_close_selected(root,all,0);
}
static bool instance_dylib_generation_close(XrXirInstance *instance) {
    return instance_dylib_close_selected(instance_root(instance),true,instance->generation_id);
}
static XirDylibFrame *instance_dylib_frame_find(XrXirInstance *root,const XrXirCallView *view) {
    for(XirDylibFrame *frame=root->dylib_frames;frame;frame=frame->next)
        if(frame->call==view->activation&&frame->frame==view->state)return frame;
    return NULL;
}
static XirDylibOwner *instance_dylib_lookup(XrXirInstance *instance,int64_t token,XirPipeQuery *query) {
    if(token<=0)return NULL;
    for(XirDylibOwner *node=instance_root(instance)->dylib_owners;node;node=node->next) {
        if(!instance_pipe_current(query))return NULL;
        if(node->token==token&&node->generation==instance->generation_id&&node->published&&!node->consumed)return node;
    }
    return NULL;
}
XR_FUNC bool xir_instance_dylib_retirement_pending(const void *instance,const XrXirCall *call,
    const XrXirValueAdmission *admission) {
    if(!instance||!admission||admission->function!=admit_function_binding)return false;
    const XrXirInstance *root=instance_root((XrXirInstance *)instance);
    for(const XirDylibFrame *frame=root->dylib_frames;frame;frame=frame->next)
        if(frame->call==call&&frame->retiring&&frame->library&&!frame->library->published&&frame->library->owner.state)return true;
    return false;
}
XR_FUNC bool xir_instance_dylib_retire(XrXirCallView *view,XrXirCallStatus *failure) {
    uint32_t entry=UINT32_MAX;const XrXirValueAdmission *admission=xir_call_retirement_admission(view,&entry);
    if(!admission||admission->function!=admit_function_binding||!view->instance)return true;
    XrXirInstance *root=instance_root(view->instance);XirDylibFrame *frame=instance_dylib_frame_find(root,view);
    if(!frame)return true;
    XR_CHECK(frame->entry==entry&&frame->generation&&admission->context==frame->generation&&
        frame->generation_id==frame->generation->generation_id&&frame->generation->domain==root->domain&&
        frame->generation->program->arena==view->arena,"loader retirement retains its actual frame and generation");
    frame->retiring=true;
    if(frame->library&&!frame->library->published) {
        bool closed=false;XrOsIoStatus status=instance_dylib_close_owner(frame->library,&closed);
        if(status!=XR_OS_IO_OK&&frame->retirement_failure==XR_XIR_CALL_READY)
            frame->retirement_failure=instance_pipe_status(status);
        if(status!=XR_OS_IO_OK||!closed)return false;
    }
    *failure=frame->retirement_failure;return true;
}
static void instance_dylib_frame_release(XrXirCallView *view) {
    if(!view||!view->instance)return;
    XrXirInstance *root=instance_root(view->instance);XirDylibFrame *frame=instance_dylib_frame_find(root,view);
    if(!frame)return;
    XR_CHECK(frame->generation&&frame->generation_id==frame->generation->generation_id&&
        frame->generation->program->arena==view->arena,"loader release retains its actual generation");
    XR_CHECK(!frame->library||frame->library->published||(!frame->library->owner.state&&!frame->library->origin),
        "an unpublished loader retires before its frame disappears");
    if(frame->library){XR_CHECK(frame->library->references,"loader frame owns one reference");--frame->library->references;}
    frame->call=NULL;frame->frame=NULL;frame->generation=NULL;frame->generation_id=0;
    XirDylibFrame **link=&root->dylib_frames;while(*link&&*link!=frame)link=&(*link)->next;
    XR_CHECK(*link==frame,"loader release retains its Root node");*link=frame->next;
    xr_xir_domain_metadata_deallocate(root->domain,frame,sizeof(*frame));
    bool driving=root->driving;root->driving=true;(void)instance_dylib_owners_close(root,false);root->driving=driving;
}
