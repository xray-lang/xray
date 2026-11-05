/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_file_metadata.inc.c - Root-owned temporary filesystem handles
 *
 * KEY CONCEPT:
 *   A failed OS close retains its real handle without retaining a Call stack.
 */
#include "../os/os_file_metadata.h"
#include "../os/os_file_temp.h"
#include "../os/os_directory_owned.h"
#include "../os/os_file_owned.h"

static XrOsIoStatus instance_temp_allocate(void *context,size_t bytes,void **output) {
    XrXirDomain *domain=context;
    if(!output || *output || !xr_xir_domain_current(domain))return XR_OS_IO_BAD_ARGUMENT;
    if(bytes>SIZE_MAX-sizeof(XirEnvironmentBlock))return XR_OS_IO_OVERFLOW;
    size_t total=sizeof(XirEnvironmentBlock)+(bytes?bytes:1);
    if(!xr_xir_domain_work(domain,total))return XR_OS_IO_MEMORY_LIMIT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirEnvironmentBlock *block=xr_xir_domain_call_allocate(domain,total,&status);
    if(!block) {
        if(status==XR_XIR_VALUE_OOM)return XR_OS_IO_OUT_OF_MEMORY;
        if(status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_REFCOUNT_LIMIT)return XR_OS_IO_MEMORY_LIMIT;
        return status==XR_XIR_VALUE_UNSUPPORTED?XR_OS_IO_UNSUPPORTED:XR_OS_IO_BAD_ARGUMENT;
    }
    block->bytes=total;*output=block+1;return XR_OS_IO_OK;
}
static void instance_temp_free(void *context,void *memory) {
    if(!memory)return;
    XirEnvironmentBlock *block=(XirEnvironmentBlock *)memory-1;
    xr_xir_domain_call_deallocate(context,block,block->bytes);
}
static XrOsIoPolicy instance_temp_policy(XrXirInstance *root) {
    return (XrOsIoPolicy){root->domain,instance_temp_allocate,instance_temp_free};
}

struct XirFileMetadataOwner {
    XrOsFileMetadataOwner owner;
    XrOsFileTempOwner temporary;
    XrOsDirectoryOwner directory;
    XrOsDirectoryText pending_name;
    uint64_t directory_token,directory_generation;
    bool directory_published,directory_closing,directory_end;
    XrOsFileOwner file;
    XirFileFrame *file_frames;
    uint64_t file_token,file_generation;
    bool file_published;
    uint32_t kind;
    XirFileMetadataOwner *next;
};
enum { XIR_FILE_METADATA_OWNER, XIR_FILE_TEMP_OWNER, XIR_DIRECTORY_OWNER, XIR_OWNED_FILE_OWNER };
static void instance_directory_name_drop(XrXirInstance *root,XirFileMetadataOwner *owner) {
    instance_temp_free(root->domain,owner->pending_name.bytes);owner->pending_name=(XrOsDirectoryText){0};
}
static void instance_file_metadata_owner_unlink(XrXirInstance *root,XirFileMetadataOwner *owner) {
    XR_CHECK(!owner->owner.handle && !owner->temporary.state && !owner->directory.state &&
        !owner->pending_name.bytes && !owner->pending_name.length && !owner->file.state && !owner->file_frames,"only a discharged filesystem owner can retire");
    XirFileMetadataOwner **link=&root->file_metadata_owners;
    while(*link && *link!=owner)link=&(*link)->next;
    XR_CHECK(*link==owner,"filesystem close requires its root-owned node");
    *link=owner->next;xr_xir_domain_metadata_deallocate(root->domain,owner,sizeof(*owner));
}
static bool instance_file_metadata_owners_close(XrXirInstance *root) {
    XR_CHECK(root==instance_root(root) && root->driving,"root close retains the busy owner");
    bool complete=true;
    for(XirFileMetadataOwner *owner=root->file_metadata_owners;owner;) {
        XirFileMetadataOwner *next=owner->next;
        XrOsIoStatus status=XR_OS_IO_BAD_ARGUMENT;
        if(owner->kind==XIR_FILE_METADATA_OWNER)status=xr_os_io_file_metadata_close(&owner->owner);
        else if(owner->kind==XIR_FILE_TEMP_OWNER) {
            XrOsIoPolicy policy=instance_temp_policy(root);
            status=xr_os_io_temp_finish(&policy,&owner->temporary,false);
        }else if(owner->kind==XIR_DIRECTORY_OWNER) {
            status=xr_os_io_directory_close(&owner->directory);
            if(status==XR_OS_IO_OK)instance_directory_name_drop(root,owner);
        }
        if(owner->kind==XIR_OWNED_FILE_OWNER) {
            bool closed=false;status=instance_file_owner_close(owner,&closed);
            if(status==XR_OS_IO_OK && !closed) {complete=false;owner=next;continue;}
        }
        if(status==XR_OS_IO_OK)
            instance_file_metadata_owner_unlink(root,owner);
        else complete=false;
        owner=next;
    }
    return complete;
}
static XrXirCallStatus instance_file_metadata_value(XrXirCallView *view,XrXirType type,
    const XrOsFileMetadata *facts,XrXirValue *output) {
    const XrXirTypes *types=xr_xir_compile_type_arena_types(view->arena);
    if(!xir_file_stat_nullable(types,type))return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    const XrXirTypeNode *optional=xr_xir_type_node(types,type);
    XrXirValue fields[10]={{0}},object={0},result={0};XrXirCallStatus status=XR_XIR_CALL_READY;
    if(facts->present) {
        XrXirType inner=optional->element;
        const XrXirTypeNode *node=xr_xir_type_node(types,inner);
        int64_t integers[]={facts->size,facts->mode,facts->mtime,facts->atime,facts->ctime};
        for(uint32_t f=0;f<5;++f)fields[f]=(XrXirValue){XR_XIR_I64,0,integers[f]};
        const XrOsOptionalI64 *identities[]={&facts->uid,&facts->gid};
        for(uint32_t i=0;i<2 && status==XR_XIR_CALL_READY;++i) {
            XrXirValue integer={XR_XIR_I64,0,identities[i]->value};
            status=value_call_status(xr_xir_nullable_new(node->nominal.fields[5+i],
                identities[i]->present?&integer:NULL,admission,&fields[5+i]));
        }
        fields[7]=(XrXirValue){XR_XIR_BOOL,0,facts->is_file};
        fields[8]=(XrXirValue){XR_XIR_BOOL,0,facts->is_dir};
        fields[9]=(XrXirValue){XR_XIR_BOOL,0,facts->is_symlink};
        if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_class_new(inner,fields,10,admission,&object));
    }
    if(status==XR_XIR_CALL_READY)status=value_call_status(xr_xir_nullable_new(type,facts->present?&object:NULL,admission,&result));
    for(uint32_t f=0;f<10;++f)xr_xir_value_drop(&fields[f]);
    xr_xir_value_drop(&object);
    if(status==XR_XIR_CALL_READY)status=xr_xir_call_execution_status(view);
    if(status==XR_XIR_CALL_READY){*output=result;result=(XrXirValue){0};}
    xr_xir_value_drop(&result);return status;
}
static XrXirCallStatus instance_file_metadata_query(XrXirCallView *view,XrXirInstance *instance,
    const char *path,size_t length,XrXirType type,XrXirValue *output) {
    XirEnvironmentQuery query={view,instance->domain,XR_XIR_CALL_READY};
    if(!instance_system_current(&query))return query.status;
    XrXirInstance *root=instance_root(instance);XrXirCallStatus status=XR_XIR_CALL_READY;
    if(!xr_xir_domain_work(root->domain,sizeof(XirFileMetadataOwner)))return XR_XIR_CALL_LIMIT;
    XirFileMetadataOwner *owner=instance_metadata_allocate(root->domain,sizeof(*owner),&status);
    if(!owner)return status;
    owner->next=root->file_metadata_owners;root->file_metadata_owners=owner;
    XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
    XrOsFileMetadata facts={0};
    XrOsIoStatus observed=xr_os_io_file_metadata(&policy,path,length,instance_system_current,&owner->owner,&facts);
    bool retained=owner->owner.handle!=0;
    if(!retained)instance_file_metadata_owner_unlink(root,owner);
    if(!instance_system_current(&query))return query.status;
    status=instance_environment_status(observed);
    if(status==XR_XIR_CALL_READY && retained)status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY)status=instance_file_metadata_value(view,type,&facts,output);
    return status;
}
