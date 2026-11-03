/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io_mutation.inc.c - Authenticated paths before filesystem effects
 *
 * KEY CONCEPT:
 *   Every path is retained and validated before an OS mutation. Failed file
 *   closes keep only their actual handle in the root's existing owner list.
 */
#include "../os/os_file_mutation.h"
typedef struct XirIoPathScan { XirEnvironmentQuery *query; size_t reads; } XirIoPathScan;
static int instance_io_path_read(void *context,const uint8_t *address,uint8_t *output) {
    XirIoPathScan *scan=context;
    if(!(scan->reads++&4095u) && !instance_environment_current(scan->query))return 0;
    if(!*address){scan->query->status=XR_XIR_CALL_HOST_ERROR;return 0;}
    *output=*address;return 1;
}
static XrXirCallStatus instance_io_mutation(XrXirCallView *view,XrXirInstance *instance,
    uint32_t id,const XrXirCallEntry *entry,const uint32_t fields[2],XrXirValue *output) {
    const XirIoNativeContract *contract=xir_io_native_contract(id);
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    XrXirValue texts[2]={{0}};XrOsPathSpan paths[2]={{0}};
    for(uint32_t p=0;p<contract->paths && query.status==XR_XIR_CALL_READY;++p) {
        if(!instance_environment_current(&query))break;
        XrXirValue base={0};
        if(!xr_xir_value_from_payload(view->arena,entry->parameters[p],view->arguments[p].payload,&base))
            query.status=XR_XIR_CALL_BAD_ARGUMENT;
        if(query.status==XR_XIR_CALL_READY)
            query.status=value_call_status(xr_xir_class_get(&base,fields[p],xr_xir_call_admission(view),texts+p));
        if(query.status!=XR_XIR_CALL_READY)break;
        if(!xr_xir_string_view(texts+p,&paths[p].text,&paths[p].length))query.status=XR_XIR_CALL_HOST_ERROR;
        if(query.status==XR_XIR_CALL_READY && (paths[p].length>INT_MAX ||
            !xr_xir_domain_work(instance->domain,(uint64_t)paths[p].length+1)))query.status=XR_XIR_CALL_LIMIT;
        if(query.status==XR_XIR_CALL_READY) {
            XirIoPathScan scan={&query,0};XrUtf8ScanResult checked={0};
            if(!xr_utf8_core_scan_strict_read((const uint8_t *)paths[p].text,paths[p].length,
                instance_io_path_read,&scan,&checked) || checked.error!=XR_UTF8_OK) {
                if(query.status==XR_XIR_CALL_READY)query.status=XR_XIR_CALL_HOST_ERROR;
            }
        }
    }
    XrXirInstance *root=instance_root(instance);XirFileMetadataOwner *owner=NULL;
    bool needs_owner=id==XR_NATIVE_CALLABLE_IO_REMOVE || id==XR_NATIVE_CALLABLE_IO_UTIME_NOW;
    if(query.status==XR_XIR_CALL_READY && needs_owner) {
        if(!xr_xir_domain_work(root->domain,sizeof(*owner)))query.status=XR_XIR_CALL_LIMIT;
        else owner=instance_metadata_allocate(root->domain,sizeof(*owner),&query.status);
        if(owner){owner->next=root->file_metadata_owners;root->file_metadata_owners=owner;}
    }
    XrOsIoStatus called=XR_OS_IO_OK;bool changed=false;
    if(query.status==XR_XIR_CALL_READY && instance_system_current(&query)) {
        XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
        switch(id) {
        case XR_NATIVE_CALLABLE_IO_MKDIR:called=xr_os_io_path_mkdir(&policy,paths[0],instance_system_current,&changed);break;
        case XR_NATIVE_CALLABLE_IO_RMDIR:called=xr_os_io_path_rmdir(&policy,paths[0],instance_system_current,&changed);break;
        case XR_NATIVE_CALLABLE_IO_REMOVE:called=xr_os_io_path_remove(&policy,paths[0],instance_system_current,&owner->owner,&changed);break;
        case XR_NATIVE_CALLABLE_IO_RENAME:called=xr_os_io_path_rename(&policy,paths[0],paths[1],instance_system_current,&changed);break;
        case XR_NATIVE_CALLABLE_IO_UTIME_NOW:called=xr_os_io_path_utime_now(&policy,paths[0],instance_system_current,&owner->owner,&changed);break;
        default:query.status=XR_XIR_CALL_BAD_STATE;break;
        }
    }
    bool retained=owner && owner->owner.handle;
    if(owner && !retained)instance_file_metadata_owner_unlink(root,owner);
    for(uint32_t p=0;p<2;++p)xr_xir_value_drop(texts+p);
    if(!instance_environment_current(&query))return query.status;
    XrXirCallStatus status=instance_environment_status(called);
    if(status==XR_XIR_CALL_READY && retained)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY)*output=(XrXirValue){XR_XIR_BOOL,0,changed?1:0};
    return status;
}
