/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * dylib_owned_win.c - Wide loader ownership and numeric thread-local errors
 */
#include "../os_dylib_owned.h"
#include "../file_metadata_path.inc.h"
#include <limits.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef struct OwnedDylibState {
    XrOsDylibOwner *anchor;
    XrOsIoPolicy memory;
    HMODULE module;
} OwnedDylibState;
typedef struct DylibWork {
    const XrOsIoPolicy *memory;
    const XrOsDylibQuery *query;
    XrOsIoStatus status;
} DylibWork;
static XR_THREAD_LOCAL DWORD dylib_thread_error;
static XrOsIoStatus dylib_error(DWORD error) {
    if(error==ERROR_NOT_ENOUGH_MEMORY||error==ERROR_OUTOFMEMORY)return XR_OS_IO_OUT_OF_MEMORY;
    if(error==ERROR_NOT_ENOUGH_QUOTA||error==ERROR_COMMITMENT_LIMIT)return XR_OS_IO_MEMORY_LIMIT;
    if(error==ERROR_ARITHMETIC_OVERFLOW)return XR_OS_IO_OVERFLOW;
    return XR_OS_IO_IO;
}
static bool dylib_refused(DWORD error) {
    return error==ERROR_MOD_NOT_FOUND||error==ERROR_DLL_NOT_FOUND||error==ERROR_PROC_NOT_FOUND||
        error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||error==ERROR_ACCESS_DENIED||
        error==ERROR_BAD_EXE_FORMAT||error==ERROR_EXE_MACHINE_TYPE_MISMATCH||error==ERROR_DLL_INIT_FAILED||
        error==ERROR_INVALID_NAME||error==ERROR_BAD_PATHNAME||error==ERROR_FILENAME_EXCED_RANGE;
}
static bool dylib_current(void *context) {
    DylibWork *work=context;
    if(work->status==XR_OS_IO_OK&&work->query&&!work->query->current(work->query->context))work->status=XR_OS_IO_CANCELLED;
    return work->status==XR_OS_IO_OK;
}
static XrOsIoStatus dylib_aligned_allocate(void *context,size_t bytes,size_t alignment,void **output) {
    DylibWork *work=context;void *memory=NULL;
    if(!dylib_current(work))return work->status;
    XrOsIoStatus status=work->memory->alloc(work->memory->context,bytes,&memory);
    if(status==XR_OS_IO_OK&&(!memory||(uintptr_t)memory%alignment))status=XR_OS_IO_BAD_ARGUMENT;
    if(status!=XR_OS_IO_OK) {
        if(memory)work->memory->free(work->memory->context,memory);
        work->status=status;return status;
    }
    *output=memory;return XR_OS_IO_OK;
}
static XrOsIoStatus dylib_allocate(void *context,size_t bytes,void **output) {
    return dylib_aligned_allocate(context,bytes,1,output);
}
static void dylib_free(void *context,void *memory) {
    DylibWork *work=context;if(memory)work->memory->free(work->memory->context,memory);
}
static OwnedDylibState *dylib_state(XrOsDylibOwner *owner) {
    OwnedDylibState *state=owner?owner->state:NULL;return state&&state->anchor==owner?state:NULL;
}
static XrOsIoStatus dylib_text(DylibWork *work,XrOsDylibText span,char **output) {
    XrOsIoPolicy bridge={work,dylib_allocate,dylib_free};
    XrOsIoStatus status=metadata_path_owned(&bridge,span.text,span.length,dylib_current,output);
    if(status!=XR_OS_IO_OK)work->status=status;
    return status;
}
static XrOsIoStatus dylib_wide(DylibWork *work,const char *text,size_t length,wchar_t **output) {
    if(length>INT_MAX)return XR_OS_IO_OVERFLOW;
    int count=length?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,(int)length,NULL,0):0;
    if(length&&!count){DWORD error=GetLastError();return error==ERROR_NO_UNICODE_TRANSLATION?XR_OS_IO_BAD_ARGUMENT:dylib_error(error);}
    if((size_t)count>SIZE_MAX/sizeof(wchar_t)-1)return XR_OS_IO_OVERFLOW;
    wchar_t *wide=NULL;XrOsIoStatus status=dylib_aligned_allocate(work,((size_t)count+1)*sizeof(*wide),_Alignof(wchar_t),(void **)&wide);
    if(status!=XR_OS_IO_OK)return status;
    if(length) {
        int converted=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,(int)length,wide,count);
        if(converted!=count){DWORD error=converted?ERROR_SUCCESS:GetLastError();status=error==ERROR_NO_UNICODE_TRANSLATION?XR_OS_IO_BAD_ARGUMENT:dylib_error(error);}
    }
    if(status==XR_OS_IO_OK&&!dylib_current(work))status=work->status;
    if(status!=XR_OS_IO_OK)dylib_free(work,wide);
    else {wide[count]=0;*output=wide;}
    return status;
}
XR_FUNC XrOsIoStatus xr_os_io_dylib_open(const XrOsIoPolicy *memory,const XrOsDylibQuery *query,
    XrOsDylibText path,XrOsDylibOwner *owner,bool *opened) {
    if(!io_policy_valid(memory)||!owner||owner->state||owner->busy||!opened||(query&&!query->current))return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;DylibWork work={memory,query,XR_OS_IO_OK};OwnedDylibState *state=NULL;
    char *text=NULL;wchar_t *wide=NULL;XrOsIoStatus status=dylib_aligned_allocate(&work,sizeof(*state),_Alignof(OwnedDylibState),(void **)&state);
    bool found=false;
    if(status==XR_OS_IO_OK) {
        *state=(OwnedDylibState){owner,*memory,NULL};owner->state=state;
        status=dylib_text(&work,path,&text);
    }
    if(status==XR_OS_IO_OK)status=dylib_wide(&work,text,path.length,&wide);
    if(status==XR_OS_IO_OK&&!dylib_current(&work))status=work.status;
    if(status==XR_OS_IO_OK) {
        SetLastError(ERROR_SUCCESS);HMODULE module=LoadLibraryW(wide);
        DWORD error=module?ERROR_SUCCESS:GetLastError();dylib_thread_error=error;
        state->module=module;found=module!=NULL;
        if(!module&&!dylib_refused(error))status=dylib_error(error);
    }
    dylib_free(&work,wide);dylib_free(&work,text);
    if(status==XR_OS_IO_OK&&!dylib_current(&work))status=work.status;
    if(status==XR_OS_IO_OK)*opened=found;
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_dylib_symbol(XrOsDylibOwner *owner,const XrOsDylibQuery *query,
    XrOsDylibText name,uintptr_t *address,bool *found) {
    OwnedDylibState *state=dylib_state(owner);
    if(!state||owner->busy||!state->module||!address||!found||(query&&!query->current))return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;DylibWork work={&state->memory,query,XR_OS_IO_OK};char *text=NULL;
    XrOsIoStatus status=dylib_text(&work,name,&text);uintptr_t value=0;bool present=false;
    if(status==XR_OS_IO_OK&&!dylib_current(&work))status=work.status;
    if(status==XR_OS_IO_OK) {
        SetLastError(ERROR_SUCCESS);FARPROC symbol=GetProcAddress(state->module,text);
        DWORD error=symbol?ERROR_SUCCESS:GetLastError();dylib_thread_error=error;
        present=symbol!=NULL;value=(uintptr_t)symbol;
        if(!symbol&&!dylib_refused(error))status=dylib_error(error);
    }
    dylib_free(&work,text);
    if(status==XR_OS_IO_OK&&!dylib_current(&work))status=work.status;
    if(status==XR_OS_IO_OK){*found=present;if(present)*address=value;}
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_dylib_close(XrOsDylibOwner *owner,bool *closed) {
    if(!owner||owner->busy||!closed)return XR_OS_IO_BAD_ARGUMENT;
    if(!owner->state){*closed=true;return XR_OS_IO_OK;}
    OwnedDylibState *state=dylib_state(owner);if(!state)return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    if(state->module) {
        SetLastError(ERROR_SUCCESS);BOOL unloaded=FreeLibrary(state->module);
        DWORD error=unloaded?ERROR_SUCCESS:GetLastError();dylib_thread_error=error;
        if(!unloaded)status=dylib_error(error);else state->module=NULL;
    }
    if(status==XR_OS_IO_OK) {
        XrOsIoPolicy memory=state->memory;owner->state=NULL;memory.free(memory.context,state);*closed=true;
    }
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_dylib_last_error(const XrOsIoPolicy *memory,
    const XrOsDylibQuery *query,XrOsDylibErrorText *output) {
    DWORD code=dylib_thread_error;
    if(!io_policy_valid(memory)||!output||output->text||output->length||output->memory.context||
        output->memory.alloc||output->memory.free||(query&&!query->current))return XR_OS_IO_BAD_ARGUMENT;
    DylibWork work={memory,query,XR_OS_IO_OK};wchar_t *wide=NULL;char *utf8=NULL;
    DWORD capacity=128,count=0;XrOsIoStatus status=XR_OS_IO_OK;
    if(code)for(;;) {
        if((size_t)capacity>SIZE_MAX/sizeof(*wide)){status=XR_OS_IO_OVERFLOW;break;}
        status=dylib_aligned_allocate(&work,(size_t)capacity*sizeof(*wide),_Alignof(wchar_t),(void **)&wide);if(status!=XR_OS_IO_OK)break;
        SetLastError(ERROR_SUCCESS);
        count=FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,NULL,code,
            MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),wide,capacity,NULL);
        DWORD error=count?ERROR_SUCCESS:GetLastError();
        if(count)break;
        dylib_free(&work,wide);wide=NULL;
        if(error!=ERROR_INSUFFICIENT_BUFFER){status=dylib_error(error);break;}
        if(capacity>MAXDWORD/2){status=XR_OS_IO_OVERFLOW;break;}capacity*=2;
    }
    if(status==XR_OS_IO_OK&&count>INT_MAX)status=XR_OS_IO_OVERFLOW;
    int bytes=0;
    if(status==XR_OS_IO_OK&&count) {
        bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,(int)count,NULL,0,NULL,NULL);
        if(!bytes)status=dylib_error(GetLastError());
    }
    if(status==XR_OS_IO_OK)status=dylib_allocate(&work,(size_t)bytes+1,(void **)&utf8);
    if(status==XR_OS_IO_OK&&count&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,(int)count,
        utf8,bytes,NULL,NULL)!=bytes)status=XR_OS_IO_IO;
    dylib_free(&work,wide);
    if(status==XR_OS_IO_OK&&!dylib_current(&work))status=work.status;
    if(status==XR_OS_IO_OK){utf8[bytes]=0;*output=(XrOsDylibErrorText){utf8,(size_t)bytes,*memory};}
    else dylib_free(&work,utf8);
    return status;
}
XR_FUNC void xr_os_io_dylib_error_free(XrOsDylibErrorText *text) {
    if(!text)return;
    if(text->text)text->memory.free(text->memory.context,text->text);
    *text=(XrOsDylibErrorText){0};
}
