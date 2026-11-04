/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * directory_owned_win.c - Wide directory handles and bounded entry batches
 *
 * KEY CONCEPT:
 *   Only a successful query supplies entries. More-data retries preserve its
 *   exact query class; successful first reads alone end restart semantics.
 */
#include "../os_directory_owned.h"
#include "../file_metadata_path.inc.h"
#include "../../base/xwindows_utf8.h"
_Static_assert(sizeof(WCHAR)==2,"Directory record names contain UTF-16 units");

typedef struct DirectoryOwned {
    XrOsIoPolicy policy;
    HANDLE handle;
    unsigned char *batch;
    size_t capacity,offset;
    XrOsIoStatus status;
    bool started,buffered,ended,closing;
} DirectoryOwned;
typedef struct DirectoryQuery {
    const XrOsIoPolicy *policy;
    void *context;
    bool (*current)(void *context);
} DirectoryQuery;
static bool directory_current(const DirectoryQuery *query) {
    return !query->current || query->current(query->context);
}
static XrOsIoStatus directory_allocate(void *context,size_t bytes,void **output) {
    DirectoryQuery *query=context;return query->policy->alloc(query->policy->context,bytes,output);
}
static void directory_free(void *context,void *memory) {
    DirectoryQuery *query=context;query->policy->free(query->policy->context,memory);
}
static bool directory_borrow_current(void *context) { return directory_current(context); }
static void *directory_memory(const XrOsIoPolicy *policy,size_t bytes,XrOsIoStatus *status) {
    void *memory=NULL;*status=policy->alloc(policy->context,bytes,&memory);
    if(*status==XR_OS_IO_OK && !memory)*status=XR_OS_IO_BAD_ARGUMENT;
    return memory;
}
XR_FUNC XrOsIoStatus xr_os_io_directory_close(XrOsDirectoryOwner *owner) {
    if(!owner)return XR_OS_IO_BAD_ARGUMENT;
    DirectoryOwned *state=owner->state;if(!state)return XR_OS_IO_OK;
    state->closing=true;
    if(state->handle!=INVALID_HANDLE_VALUE && !CloseHandle(state->handle))return io_windows_status(GetLastError());
    XrOsIoPolicy policy=state->policy;
    if(state->batch)policy.free(policy.context,state->batch);
    policy.free(policy.context,state);owner->state=NULL;return XR_OS_IO_OK;
}
static XrOsIoStatus directory_locator(const DirectoryQuery *query,XrOsPathSpan path,
    wchar_t **wide,bool *refused) {
    XrOsIoPolicy temporary={ (void *)query,directory_allocate,directory_free };
    char *raw=NULL;XrOsIoStatus status=metadata_path_owned(&temporary,path.text,path.length,directory_borrow_current,&raw);
    if(status!=XR_OS_IO_OK)return status;
    if(!path.length || path.length>(size_t)32767*3)*refused=true;
    else {
        status=xr_win_utf8_path_owned_classified(&temporary,raw,wide,refused);
        if(*refused)status=XR_OS_IO_OK;
        if(status==XR_OS_IO_OK && !*refused && !*wide)status=XR_OS_IO_BAD_ARGUMENT;
    }
    temporary.free(temporary.context,raw);
    return status==XR_OS_IO_OK && !directory_current(query)?XR_OS_IO_CANCELLED:status;
}
XR_FUNC XrOsIoStatus xr_os_io_directory_open(const XrOsDirectoryRequest *request,
    XrOsDirectoryOwner *owner,bool *opened) {
    if(!request || !owner || owner->state || !opened || !io_policy_valid(request->policy))return XR_OS_IO_BAD_ARGUMENT;
    DirectoryQuery query={request->policy,request->context,request->current};
    if(!directory_current(&query))return XR_OS_IO_CANCELLED;
    wchar_t *wide=NULL;bool refused=false;
    XrOsIoStatus status=directory_locator(&query,request->path,&wide,&refused);
    DirectoryOwned *state=NULL;
    if(status==XR_OS_IO_OK && !refused) {
        state=directory_memory(request->policy,sizeof(*state),&status);
        if(state) {
            if((uintptr_t)state%_Alignof(DirectoryOwned)) {request->policy->free(request->policy->context,state);state=NULL;status=XR_OS_IO_BAD_ARGUMENT;}
            else {memset(state,0,sizeof(*state));state->policy=*request->policy;state->handle=INVALID_HANDLE_VALUE;owner->state=state;}
        }
    }
    if(status==XR_OS_IO_OK && !refused && !directory_current(&query))status=XR_OS_IO_CANCELLED;
    if(status==XR_OS_IO_OK && !refused) {
        state->handle=CreateFileW(wide,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
        if(state->handle==INVALID_HANDLE_VALUE) {
            DWORD error=GetLastError();refused=xr_win_path_error_refused(error);
            if(!refused)status=io_windows_status(error);
        }else if(!directory_current(&query))status=XR_OS_IO_CANCELLED;
        else {
            FILE_STANDARD_INFO information={0};
            if(!GetFileInformationByHandleEx(state->handle,FileStandardInfo,&information,sizeof(information)))status=io_windows_status(GetLastError());
            else refused=!information.Directory;
        }
    }
    if(wide)request->policy->free(request->policy->context,wide);
    if(status==XR_OS_IO_OK && !directory_current(&query))status=XR_OS_IO_CANCELLED;
    if(status!=XR_OS_IO_OK || refused) {
        XrOsIoStatus closed=xr_os_io_directory_close(owner);
        if(status==XR_OS_IO_OK)status=closed;
    }
    if(status==XR_OS_IO_OK)*opened=!refused;
    return status;
}
static XrOsIoStatus directory_resize(DirectoryOwned *state) {
    const size_t maximum=(size_t)UINT32_MAX&~(size_t)7;
    if(state->capacity>=maximum)return XR_OS_IO_OVERFLOW;
    size_t capacity=state->capacity?(state->capacity>maximum/2?maximum:state->capacity*2):4096;
    XrOsIoStatus status=XR_OS_IO_OK;
    unsigned char *batch=directory_memory(&state->policy,capacity,&status);
    if(status!=XR_OS_IO_OK)return status;
    if((uintptr_t)batch%8u){state->policy.free(state->policy.context,batch);return XR_OS_IO_BAD_ARGUMENT;}
    if(state->batch)state->policy.free(state->policy.context,state->batch);
    state->batch=batch;state->capacity=capacity;return XR_OS_IO_OK;
}
static XrOsIoStatus directory_validate(DirectoryOwned *state,const DirectoryQuery *query) {
    const size_t header=offsetof(FILE_ID_BOTH_DIR_INFO,FileName);size_t offset=0;
    for(;;) {
        if(!directory_current(query))return XR_OS_IO_CANCELLED;
        size_t remaining=state->capacity-offset;
        if(remaining<header)return XR_OS_IO_BAD_ARGUMENT;
        const FILE_ID_BOTH_DIR_INFO *entry=(const void *)(state->batch+offset);
        size_t next=entry->NextEntryOffset,span=next?next:remaining;
        if((next && (next%8u || next<header || next>remaining)) || !entry->FileNameLength ||
            entry->FileNameLength%sizeof(WCHAR) || entry->FileNameLength>span-header)return XR_OS_IO_BAD_ARGUMENT;
        size_t units=entry->FileNameLength/sizeof(WCHAR);
        for(size_t n=0;n<units;++n) {
            if(!(n&4095u) && !directory_current(query))return XR_OS_IO_CANCELLED;
            uint16_t value=entry->FileName[n];if(!value)return XR_OS_IO_BAD_ARGUMENT;
            if(value>=0xD800 && value<=0xDBFF) {
                if(++n==units || entry->FileName[n]<0xDC00 || entry->FileName[n]>0xDFFF)return XR_OS_IO_BAD_ARGUMENT;
            }else if(value>=0xDC00 && value<=0xDFFF)return XR_OS_IO_BAD_ARGUMENT;
        }
        if(!next)return XR_OS_IO_OK;
        offset+=next;
    }
}
static XrOsIoStatus directory_fetch(DirectoryOwned *state,const DirectoryQuery *query) {
    for(;;) {
        if(!directory_current(query))return XR_OS_IO_CANCELLED;
        if(!state->batch) {XrOsIoStatus status=directory_resize(state);if(status!=XR_OS_IO_OK)return status;}
        for(size_t offset=0;offset<state->capacity;) {
            if(!directory_current(query))return XR_OS_IO_CANCELLED;
            size_t count=state->capacity-offset<4096?state->capacity-offset:4096;
            memset(state->batch+offset,0,count);offset+=count;
        }
        FILE_INFO_BY_HANDLE_CLASS kind=state->started?FileIdBothDirectoryInfo:FileIdBothDirectoryRestartInfo;
        if(GetFileInformationByHandleEx(state->handle,kind,state->batch,(DWORD)state->capacity)) {
            XrOsIoStatus status=directory_validate(state,query);if(status!=XR_OS_IO_OK)return status;
            state->started=true;state->buffered=true;state->offset=0;return XR_OS_IO_OK;
        }
        DWORD error=GetLastError();
        if(error==ERROR_NO_MORE_FILES){state->ended=true;return XR_OS_IO_END;}
        if(error!=ERROR_MORE_DATA)return io_windows_status(error);
        /* The documented same-API sample retries the unchanged information
         * class after more-data; a successful restart alone switches classes. */
        XrOsIoStatus status=directory_resize(state);if(status!=XR_OS_IO_OK)return status;
    }
}
static XrOsIoStatus directory_text(DirectoryOwned *state,const DirectoryQuery *query,
    const FILE_ID_BOTH_DIR_INFO *entry,XrOsDirectoryText *output) {
    size_t units=entry->FileNameLength/sizeof(WCHAR);if(units>INT_MAX)return XR_OS_IO_OVERFLOW;
    int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,entry->FileName,(int)units,NULL,0,NULL,NULL);
    if(!bytes)return io_windows_status(GetLastError());
    XrOsIoStatus status=XR_OS_IO_OK;char *text=directory_memory(&state->policy,(size_t)bytes+1,&status);
    if(!text)return status;
    if(!directory_current(query))status=XR_OS_IO_CANCELLED;
    else if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,entry->FileName,(int)units,text,bytes,NULL,NULL)!=bytes)
        status=XR_OS_IO_BAD_ARGUMENT;
    if(status==XR_OS_IO_OK && !directory_current(query))status=XR_OS_IO_CANCELLED;
    if(status!=XR_OS_IO_OK)state->policy.free(state->policy.context,text);
    else {text[bytes]=0;*output=(XrOsDirectoryText){text,(size_t)bytes};}
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_directory_next(XrOsDirectoryOwner *owner,
    void *context,bool (*current)(void *),XrOsDirectoryText *output) {
    if(!owner || !owner->state || !output || output->bytes || output->length)return XR_OS_IO_BAD_ARGUMENT;
    DirectoryOwned *state=owner->state;
    if(state->closing)return XR_OS_IO_BAD_ARGUMENT;
    if(state->status!=XR_OS_IO_OK)return state->status;
    DirectoryQuery query={&state->policy,context,current};XrOsIoStatus status=XR_OS_IO_OK;
    for(;;) {
        if(!directory_current(&query)){status=XR_OS_IO_CANCELLED;break;}
        if(state->ended)return XR_OS_IO_END;
        if(!state->buffered){status=directory_fetch(state,&query);if(status!=XR_OS_IO_OK)break;}
        const FILE_ID_BOTH_DIR_INFO *entry=(const void *)(state->batch+state->offset);
        size_t units=entry->FileNameLength/sizeof(WCHAR),next=entry->NextEntryOffset;
        bool dot=entry->FileName[0]==L'.' && (units==1 || (units==2 && entry->FileName[1]==L'.'));
        if(!dot){status=directory_text(state,&query,entry,output);if(status!=XR_OS_IO_OK)break;}
        if(next)state->offset+=next;else state->buffered=false;
        if(!dot)return XR_OS_IO_OK;
    }
    if(status!=XR_OS_IO_END)state->status=status;
    return status;
}
