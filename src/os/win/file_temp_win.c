/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_temp_win.c - Handle-bound exclusive temporary nodes
 *
 * KEY CONCEPT:
 *   FILE_CREATE returns the actual new node. Rollback uses its still-owned
 *   handle, never a later lookup of an externally replaceable path.
 */
#include "../os_file_temp.h"
#include "../os_random.h"
#include "../file_metadata_path.inc.h"
#include "../../base/xwindows_utf8.h"
#include <winternl.h>

typedef NTSTATUS (NTAPI *FileTempCreate)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK,PLARGE_INTEGER,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
typedef ULONG (NTAPI *FileTempError)(NTSTATUS);
typedef struct FileTempApi { FileTempCreate create; FileTempError error; } FileTempApi;
typedef struct FileTempState {
    XrOsIoPolicy allocation;
    char *candidate;
    HANDLE parent,child;
    bool created,ready,rollback,delete_marked;
} FileTempState;
typedef struct FileTempWork { const XrOsFileTempRequest *request; XrOsIoStatus status; } FileTempWork;

static bool file_temp_current(void *context) {
    FileTempWork *work=context;
    if(work->status==XR_OS_IO_OK && work->request->current && !work->request->current(work->request->context))
        work->status=XR_OS_IO_CANCELLED;
    return work->status==XR_OS_IO_OK;
}
static XrOsIoStatus file_temp_allocate(void *context,size_t bytes,void **output) {
    FileTempWork *work=context;
    if(!file_temp_current(work))return work->status;
    const XrOsIoPolicy *policy=work->request->policy;
    XrOsIoStatus status=policy->alloc(policy->context,bytes,output);
    if(status==XR_OS_IO_OK && !*output)status=XR_OS_IO_BAD_ARGUMENT;
    return status;
}
static void file_temp_free(void *context,void *memory) {
    FileTempWork *work=context;const XrOsIoPolicy *policy=work->request->policy;
    if(memory)policy->free(policy->context,memory);
}
static bool file_temp_api(FileTempApi *api) {
    HMODULE module=GetModuleHandleW(L"ntdll.dll");
    if(!module)return false;
    FARPROC create=GetProcAddress(module,"NtCreateFile"),error=GetProcAddress(module,"RtlNtStatusToDosError");
    _Static_assert(sizeof(create)==sizeof(api->create),"Windows function address size");
    _Static_assert(sizeof(error)==sizeof(api->error),"Windows function address size");
    memcpy(&api->create,&create,sizeof(create));memcpy(&api->error,&error,sizeof(error));
    return api->create && api->error;
}
static bool file_temp_refused(DWORD error) {
    return xr_win_path_error_refused(error) || error==ERROR_DIRECTORY || error==ERROR_DISK_FULL ||
        error==ERROR_HANDLE_DISK_FULL || error==ERROR_WRITE_PROTECT;
}
static XrOsIoStatus file_temp_close(HANDLE *handle) {
    if(!*handle)return XR_OS_IO_OK;
    if(!CloseHandle(*handle))return XR_OS_IO_IO;
    *handle=NULL;return XR_OS_IO_OK;
}
static bool file_temp_policy(const FileTempState *state,const XrOsIoPolicy *policy) {
    return io_policy_valid(policy) && state->allocation.context==policy->context &&
        state->allocation.alloc==policy->alloc && state->allocation.free==policy->free;
}
XR_FUNC XrOsIoStatus xr_os_io_temp_finish(const XrOsIoPolicy *policy,XrOsFileTempOwner *owner,bool keep) {
    if(!owner || !io_policy_valid(policy))return XR_OS_IO_BAD_ARGUMENT;
    FileTempState *state=owner->state;if(!state)return XR_OS_IO_OK;
    if(!file_temp_policy(state,policy) || (keep && (!state->ready || state->rollback)))return XR_OS_IO_BAD_ARGUMENT;
    if(keep) {
        XrOsIoStatus status=file_temp_close(&state->parent);
        if(status==XR_OS_IO_OK)status=file_temp_close(&state->child);
        if(status!=XR_OS_IO_OK){state->rollback=true;return status;}
    } else {
        state->rollback=true;
        if(state->created && !state->delete_marked) {
            if(!state->child)return XR_OS_IO_IO;
            FILE_DISPOSITION_INFO remove={TRUE};
            if(!SetFileInformationByHandle(state->child,FileDispositionInfo,&remove,sizeof(remove)))return XR_OS_IO_IO;
            state->delete_marked=true;
        }
        XrOsIoStatus status=file_temp_close(&state->child);
        if(status!=XR_OS_IO_OK)return status;
        status=file_temp_close(&state->parent);if(status!=XR_OS_IO_OK)return status;
    }
    if(state->candidate)policy->free(policy->context,state->candidate);
    policy->free(policy->context,state);owner->state=NULL;return XR_OS_IO_OK;
}
static XrOsIoStatus file_temp_create(const XrOsFileTempRequest *request,
    XrOsFileTempOwner *owner,bool *output,bool directory) {
    if(!request || !owner || owner->state || !output || !request->prepare ||
        !io_policy_valid(request->policy) || !request->root.text)return XR_OS_IO_BAD_ARGUMENT;
    FileTempApi api={0};if(!file_temp_api(&api))return XR_OS_IO_UNSUPPORTED;
    FileTempWork work={request,XR_OS_IO_OK};XrOsIoPolicy proxy={&work,file_temp_allocate,file_temp_free};
    XrIoContext io={&proxy,XR_OS_IO_OK};char *root=NULL;wchar_t *locator=NULL;bool refused=false;
    XrOsIoStatus status=metadata_path_owned(&proxy,request->root.text,request->root.length,file_temp_current,&root);
    if(status==XR_OS_IO_OK && request->root.length>(size_t)32767*3)refused=true;
    if(status==XR_OS_IO_OK && !refused && request->root.length)
        status=xr_win_utf8_path_owned_classified(&proxy,root,&locator,&refused);
    if(refused)status=XR_OS_IO_OK;
    FileTempState *state=NULL;
    if(status==XR_OS_IO_OK && !refused && request->root.length && file_temp_current(&work)) {
        if(request->root.length>SIZE_MAX-37 || request->root.length>(size_t)INT_MAX-37)status=XR_OS_IO_OVERFLOW;
        else {
            state=io_alloc(&io,sizeof(*state));
            if(state) {
                memset(state,0,sizeof(*state));state->allocation=*request->policy;owner->state=state;
                state->candidate=io_alloc(&io,request->root.length+37);
            }
            if(!state || !state->candidate)status=io.status==XR_OS_IO_OK?XR_OS_IO_BAD_ARGUMENT:io.status;
        }
    }
    if(status==XR_OS_IO_OK && state && file_temp_current(&work)) {
        DWORD access=FILE_READ_ATTRIBUTES|SYNCHRONIZE|(directory?FILE_ADD_SUBDIRECTORY:FILE_ADD_FILE);
        state->parent=CreateFileW(locator,access,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(state->parent==INVALID_HANDLE_VALUE) {
            state->parent=NULL;DWORD error=GetLastError();
            if(file_temp_refused(error))refused=true;else status=io_windows_status(error);
        }
    }
    if(status==XR_OS_IO_OK && state && !refused && state->parent) {
        size_t prefix=request->root.length;memcpy(state->candidate,root,prefix);
        if(prefix && root[prefix-1]!='/' && root[prefix-1]!='\\')state->candidate[prefix++]='/';
        for(unsigned attempt=0;attempt<32 && file_temp_current(&work);++attempt) {
            unsigned char entropy[16];wchar_t leaf[36];static const char hex[]="0123456789abcdef";
            xr_random_bytes(entropy,sizeof(entropy));memcpy(state->candidate+prefix,"xr-",3);
            for(size_t n=0;n<sizeof(entropy);++n) {
                state->candidate[prefix+3+n*2]=hex[entropy[n]>>4];
                state->candidate[prefix+4+n*2]=hex[entropy[n]&15];
            }
            state->candidate[prefix+35]=0;
            for(size_t n=0;n<35;++n)leaf[n]=(wchar_t)(unsigned char)state->candidate[prefix+n];leaf[35]=0;
            status=request->prepare(request->context,state->candidate,prefix+35);
            if(status!=XR_OS_IO_OK || !file_temp_current(&work))break;
            UNICODE_STRING name={35*(USHORT)sizeof(wchar_t),36*(USHORT)sizeof(wchar_t),leaf};
            OBJECT_ATTRIBUTES attributes={sizeof(attributes),state->parent,&name,OBJ_CASE_INSENSITIVE,NULL,NULL};
            IO_STATUS_BLOCK observed={0};
            ACCESS_MASK desired=DELETE|FILE_READ_ATTRIBUTES|SYNCHRONIZE|(directory?FILE_LIST_DIRECTORY:FILE_WRITE_DATA);
            NTSTATUS called=api.create(&state->child,desired,&attributes,&observed,NULL,FILE_ATTRIBUTE_NORMAL,
                FILE_SHARE_READ|FILE_SHARE_WRITE,FILE_CREATE,FILE_SYNCHRONOUS_IO_NONALERT|
                (directory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE),NULL,0);
            if(called>=0) {
                if(!state->child || state->child==INVALID_HANDLE_VALUE || observed.Information!=FILE_CREATED)status=XR_OS_IO_IO;
                else {state->created=true;state->ready=true;}
                break;
            }
            if(state->child){status=XR_OS_IO_IO;break;}
            DWORD error=api.error(called);
            if(error==ERROR_FILE_EXISTS || error==ERROR_ALREADY_EXISTS)continue;
            if(file_temp_refused(error))refused=true;else status=io_windows_status(error);
            break;
        }
    }
    if(status==XR_OS_IO_OK && !file_temp_current(&work))status=work.status;
    io_free(&io,locator);io_free(&io,root);
    if(status==XR_OS_IO_OK && state && state->created){*output=true;return XR_OS_IO_OK;}
    XrOsIoStatus cleaned=xr_os_io_temp_finish(request->policy,owner,false);
    if(status==XR_OS_IO_OK)status=cleaned;
    if(status==XR_OS_IO_OK)*output=false;
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_temp_directory(const XrOsFileTempRequest *request,XrOsFileTempOwner *owner,bool *created) {
    return file_temp_create(request,owner,created,true);
}
XR_FUNC XrOsIoStatus xr_os_io_temp_file(const XrOsFileTempRequest *request,XrOsFileTempOwner *owner,bool *created) {
    return file_temp_create(request,owner,created,false);
}
