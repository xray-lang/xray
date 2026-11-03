/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io_links.inc.c - Authenticated Path reads and owned link text
 *
 * KEY CONCEPT:
 *   Both Path references remain owned throughout the OS call. A failed close
 *   retains only its actual root-owned handle; text is published atomically.
 */
#include "../os/os_file_links.h"

static XrXirCallStatus instance_io_links(XrXirCallView *view,XrXirInstance *instance,
    uint32_t id,const XrXirCallEntry *entry,const uint32_t fields[2],XrXirValue *output) {
    bool symlink=id==XR_NATIVE_CALLABLE_IO_SYMLINK;
    if(!symlink && id!=XR_NATIVE_CALLABLE_IO_READLINK && id!=XR_NATIVE_CALLABLE_IO_REALPATH)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(view->arena);
    if(symlink?entry->result!=XR_XIR_BOOL:!xir_io_nullable_string(types,entry->result))return XR_XIR_CALL_BAD_STATE;
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    XrXirValue texts[2]={{0}},text={0},result={0};XrOsPathSpan paths[2]={{0}};
    uint32_t count=symlink?2u:1u;
    for(uint32_t p=0;p<count && query.status==XR_XIR_CALL_READY;++p) {
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
    if(query.status==XR_XIR_CALL_READY && !symlink) {
        if(!xr_xir_domain_work(root->domain,sizeof(*owner)))query.status=XR_XIR_CALL_LIMIT;
        else owner=instance_metadata_allocate(root->domain,sizeof(*owner),&query.status);
        if(owner){owner->next=root->file_metadata_owners;root->file_metadata_owners=owner;}
    }
    XrOsIoStatus called=XR_OS_IO_OK;XrOsFileLinkText raw={0};bool changed=false;
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
    if(query.status==XR_XIR_CALL_READY && instance_system_current(&query)) {
        if(symlink)called=xr_os_io_path_symlink(&policy,paths[0],paths[1],instance_system_current,&changed);
        else if(id==XR_NATIVE_CALLABLE_IO_READLINK)
            called=xr_os_io_path_readlink(&policy,paths[0],instance_system_current,&owner->owner,&raw);
        else called=xr_os_io_path_realpath(&policy,paths[0],instance_system_current,&owner->owner,&raw);
    }
    bool retained=owner && owner->owner.handle;
    if(owner && !retained)instance_file_metadata_owner_unlink(root,owner);
    for(uint32_t p=0;p<2;++p)xr_xir_value_drop(texts+p);
    if(instance_environment_current(&query))query.status=instance_environment_status(called);
    if(query.status==XR_XIR_CALL_READY && retained)query.status=XR_XIR_CALL_HOST_ERROR;
    if(query.status==XR_XIR_CALL_READY && !raw.text && raw.length)query.status=XR_XIR_CALL_HOST_ERROR;
    if(query.status==XR_XIR_CALL_READY && !symlink && raw.text) {
        XrXirValueStatus copied=xr_xir_string_new(instance->domain,raw.text,raw.length,&text);
        query.status=copied==XR_XIR_VALUE_BAD_UTF8?XR_XIR_CALL_HOST_ERROR:value_call_status(copied);
    }
    instance_environment_free(&query,raw.text);
    if(query.status==XR_XIR_CALL_READY) {
        if(symlink)result=(XrXirValue){XR_XIR_BOOL,0,changed?1:0};
        else query.status=value_call_status(xr_xir_nullable_new(entry->result,text.type?&text:NULL,
            xr_xir_call_admission(view),&result));
    }
    xr_xir_value_drop(&text);
    if(instance_environment_current(&query)){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return query.status;
}
