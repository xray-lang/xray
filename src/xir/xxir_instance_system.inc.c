/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_system.inc.c - Domain-owned system facts and failed process closes
 *
 * KEY CONCEPT:
 *   Temporary handles outlive failed calls only in the root's private owner list.
 */
#include "../os/os_system.h"

struct XirSystemOwner {
    XrOsSystemOwner owner;
    XirSystemOwner *next;
};
static void instance_system_owner_unlink(XrXirInstance *root,XirSystemOwner *owner) {
    XR_CHECK(!owner->owner.handle,"only a discharged process owner can retire");
    XirSystemOwner **link=&root->system_owners;
    while(*link && *link!=owner)link=&(*link)->next;
    XR_CHECK(*link==owner,"system close requires its root-owned node");
    *link=owner->next;xr_xir_domain_metadata_deallocate(root->domain,owner,sizeof(*owner));
}
static bool instance_system_owners_close(XrXirInstance *root) {
    XR_CHECK(root==instance_root(root) && root->driving,"root close retains the busy owner");
    bool complete=true;
    for(XirSystemOwner *owner=root->system_owners;owner;) {
        XirSystemOwner *next=owner->next;
        if(xr_os_io_system_close(&owner->owner)==XR_OS_IO_OK)instance_system_owner_unlink(root,owner);
        else complete=false;
        owner=next;
    }
    return complete;
}
static bool instance_system_current(void *context) {
    XirEnvironmentQuery *query=context;
    if(!instance_environment_current(query))return false;
    if(!xr_xir_domain_work(query->domain,1))query->status=XR_XIR_CALL_LIMIT;
    return query->status==XR_XIR_CALL_READY;
}
static XrXirCallStatus instance_system_query(XrXirCallView *view,XrXirInstance *instance,
    uint32_t id,XrXirType type,XrXirValue *output) {
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_system_current(&query))return query.status;
    XrOsIoStatus observed=XR_OS_IO_UNSUPPORTED;
    XrOsOptionalI64 identity={0};int64_t integer=0;double seconds=0;bool changed=false;
    XrXirInstance *root=instance_root(instance);XirSystemOwner *owner=NULL;
    if(id==XR_NATIVE_CALLABLE_OS_PPID || id==XR_NATIVE_CALLABLE_OS_KILL ||
        id==XR_NATIVE_CALLABLE_OS_FREE_MEMORY) {
        XrXirCallStatus allocated=XR_XIR_CALL_READY;
        if(!xr_xir_domain_work(root->domain,sizeof(*owner)))return XR_XIR_CALL_LIMIT;
        owner=instance_metadata_allocate(root->domain,sizeof(*owner),&allocated);
        if(!owner)return allocated;
        owner->next=root->system_owners;root->system_owners=owner;
    }
    switch(id) {
    case XR_NATIVE_CALLABLE_OS_UID:observed=xr_os_io_uid(&identity);break;
    case XR_NATIVE_CALLABLE_OS_GID:observed=xr_os_io_gid(&identity);break;
    case XR_NATIVE_CALLABLE_OS_PPID:
        observed=xr_os_io_ppid(&owner->owner,&query,instance_system_current,&integer);break;
    case XR_NATIVE_CALLABLE_OS_KILL:
        observed=xr_os_io_kill(&owner->owner,view->arguments[0].payload,view->arguments[1].payload,
            &query,instance_system_current,&changed);break;
    case XR_NATIVE_CALLABLE_OS_TOTAL_MEMORY:observed=xr_os_io_total_memory(&integer);break;
    case XR_NATIVE_CALLABLE_OS_FREE_MEMORY:observed=xr_os_io_free_memory(&owner->owner,&integer);break;
    case XR_NATIVE_CALLABLE_OS_UPTIME:observed=xr_os_io_uptime(&seconds);break;
    default:break;
    }
    /* A provider already made its one close attempt. Retain a failed handle
     * without retrying here, including after cancellation or another failure. */
    bool retained=owner && owner->owner.handle;
    if(owner && !retained)instance_system_owner_unlink(root,owner);
    XrXirCallStatus status=query.status;XrXirValue result={0};
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY)status=instance_environment_status(observed);
    if(status==XR_XIR_CALL_READY && retained)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY) {
        if(id==XR_NATIVE_CALLABLE_OS_UID || id==XR_NATIVE_CALLABLE_OS_GID) {
            XrXirValue payload={XR_XIR_I64,0,identity.value};
            status=value_call_status(xr_xir_nullable_new(type,identity.present?&payload:NULL,
                xr_xir_call_admission(view),&result));
        } else if(id==XR_NATIVE_CALLABLE_OS_KILL)result=(XrXirValue){XR_XIR_BOOL,0,changed};
        else if(id==XR_NATIVE_CALLABLE_OS_UPTIME) {
            int64_t bits=0;memcpy(&bits,&seconds,sizeof(bits));result=(XrXirValue){XR_XIR_F64,0,bits};
        } else result=(XrXirValue){XR_XIR_I64,0,integer};
    }
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return status;
}
