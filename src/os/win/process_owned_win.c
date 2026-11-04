/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * process_owned_win.c - Suspended child publication and retryable job retirement
 */
#include "../os_process_owned.h"
#include "../os_proc.h"
#include "../file_metadata_path.inc.h"
#include <limits.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "proc_text_win.inc.c"

#define OWNED_PROCESS_KILLED ((DWORD)0xE0000001u)
typedef struct OwnedProcessState {
    XrOsProcessOwner *anchor;
    XrOsIoPolicy memory;
    HANDLE process,thread,job,streams[3];
    LPWCH inherited;
    bool prepared,started,detached,independent,terminating,assigned;
} OwnedProcessState;
_Static_assert(_Alignof(OwnedProcessState)>=_Alignof(void *)&&_Alignof(OwnedProcessState)>=_Alignof(wchar_t),
    "process allocations align state, pointer tables and UTF-16 text");
typedef struct OwnedProcessBuild {
    const XrOsIoPolicy *memory;
    const XrOsProcessQuery *query;
    XrOsIoStatus status;
} OwnedProcessBuild;
static XrOsIoStatus owned_process_error(DWORD error) {
    if(error==ERROR_NOT_ENOUGH_MEMORY||error==ERROR_OUTOFMEMORY)return XR_OS_IO_OUT_OF_MEMORY;
    if(error==ERROR_NOT_ENOUGH_QUOTA)return XR_OS_IO_MEMORY_LIMIT;
    if(error==ERROR_ARITHMETIC_OVERFLOW)return XR_OS_IO_OVERFLOW;
    return XR_OS_IO_IO;
}
static bool owned_process_refusal(DWORD error) {
    return error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND||error==ERROR_DIRECTORY||
        error==ERROR_ACCESS_DENIED||error==ERROR_BAD_EXE_FORMAT||error==ERROR_EXE_MACHINE_TYPE_MISMATCH||
        error==ERROR_INVALID_NAME||error==ERROR_BAD_PATHNAME||error==ERROR_FILENAME_EXCED_RANGE;
}
static OwnedProcessState *owned_process_state(XrOsProcessOwner *owner) {
    OwnedProcessState *state=owner?owner->state:NULL;return state&&state->anchor==owner?state:NULL;
}
static bool owned_process_current(void *context) {
    OwnedProcessBuild *build=context;
    if(build->status==XR_OS_IO_OK&&build->query&&!build->query->current(build->query->context))
        build->status=XR_OS_IO_CANCELLED;
    return build->status==XR_OS_IO_OK;
}
static XrOsIoStatus owned_process_allocate(void *context,size_t bytes,void **output) {
    OwnedProcessBuild *build=context;void *memory=NULL;
    if(!owned_process_current(build))return build->status;
    XrOsIoStatus status=build->memory->alloc(build->memory->context,bytes,&memory);
    if(status==XR_OS_IO_OK&&(!memory||(uintptr_t)memory%_Alignof(OwnedProcessState)))status=XR_OS_IO_BAD_ARGUMENT;
    if(status!=XR_OS_IO_OK) {
        if(memory)build->memory->free(build->memory->context,memory);
        build->status=status;return status;
    }
    *output=memory;return XR_OS_IO_OK;
}
static void owned_process_free(void *context,void *memory) {
    OwnedProcessBuild *build=context;if(memory)build->memory->free(build->memory->context,memory);
}
static XrOsProcStatus owned_process_text_allocate(void *context,size_t bytes,void **output) {
    XrOsIoStatus status=owned_process_allocate(context,bytes,output);
    return status==XR_OS_IO_OK?XR_PROC_OK:status==XR_OS_IO_OUT_OF_MEMORY?XR_PROC_OUT_OF_MEMORY:
        status==XR_OS_IO_MEMORY_LIMIT?XR_PROC_MEMORY_LIMIT:status==XR_OS_IO_OVERFLOW?XR_PROC_OVERFLOW:XR_PROC_IO;
}
static XrOsIoStatus owned_process_text_status(XrOsProcStatus status) {
    return status==XR_PROC_OK?XR_OS_IO_OK:status==XR_PROC_OUT_OF_MEMORY?XR_OS_IO_OUT_OF_MEMORY:
        status==XR_PROC_MEMORY_LIMIT?XR_OS_IO_MEMORY_LIMIT:
        status==XR_PROC_OVERFLOW||status==XR_PROC_BUDGET?XR_OS_IO_OVERFLOW:
        status==XR_PROC_INVALID_ARGUMENT?XR_OS_IO_BAD_ARGUMENT:XR_OS_IO_IO;
}
static char *owned_process_text(OwnedProcessBuild *build,XrOsProcessText text) {
    XrOsIoPolicy bridge={build,owned_process_allocate,owned_process_free};char *output=NULL;
    XrOsIoStatus status=metadata_path_owned(&bridge,text.text,text.length,owned_process_current,&output);
    if(status!=XR_OS_IO_OK)build->status=status;
    return output;
}
#include "process_shell_win.inc.c"
static XrOsIoStatus owned_process_prepare(const XrOsIoPolicy *memory,
    const XrOsProcessQuery *query,const XrOsProcessRequest *request,
    const XrOsProcessText *shell,XrOsProcessOwner *owner,bool *created) {
    if(!memory||!memory->context||!memory->alloc||!memory->free||!request||!owner||owner->state||owner->busy||
        !created||(query&&!query->current)||(request->argument_count&&!request->arguments)||
        (request->environment_count&&(!request->env_keys||!request->env_values)))return XR_OS_IO_BAD_ARGUMENT;
    if(shell&&(request->program.text||request->program.length||request->arguments||request->argument_count||request->detached))
        return XR_OS_IO_BAD_ARGUMENT;
    for(uint32_t i=0;i<3;++i)if(request->redirected[i]&&
        (!request->streams[i]||request->streams[i]==(intptr_t)INVALID_HANDLE_VALUE))return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;OwnedProcessBuild build={memory,query,XR_OS_IO_OK};OwnedProcessState *state=NULL;
    XrOsIoStatus status=owned_process_allocate(&build,sizeof(*state),(void **)&state);
    char **texts=NULL;wchar_t *program=NULL,*command=NULL,*cwd=NULL,*environment=NULL;
    STARTUPINFOEXW startup={0};bool attributes=false,refused=false;
    size_t count=0;
    if(status!=XR_OS_IO_OK)goto done;
    memset(state,0,sizeof(*state));state->anchor=owner;state->memory=*memory;state->detached=request->detached;owner->state=state;
    if(request->argument_count>SIZE_MAX-3||request->environment_count>(SIZE_MAX-request->argument_count-3)/2) {
        build.status=XR_OS_IO_OVERFLOW;goto done;
    }
    count=request->argument_count+3+request->environment_count*2;
    if(count>SIZE_MAX/sizeof(*texts)){build.status=XR_OS_IO_OVERFLOW;goto done;}
    status=owned_process_allocate(&build,count*sizeof(*texts),(void **)&texts);if(status!=XR_OS_IO_OK)goto done;
    memset(texts,0,count*sizeof(*texts));texts[0]=owned_process_text(&build,shell?*shell:request->program);
    for(size_t i=0;i<request->argument_count&&build.status==XR_OS_IO_OK;++i)
        texts[i+1]=owned_process_text(&build,request->arguments[i]);
    size_t at=request->argument_count+2;
    if(request->cwd&&build.status==XR_OS_IO_OK)texts[at]=owned_process_text(&build,*request->cwd);
    ++at;
    for(size_t i=0;i<request->environment_count&&build.status==XR_OS_IO_OK;++i) {
        texts[at+i*2]=owned_process_text(&build,request->env_keys[i]);
        texts[at+i*2+1]=owned_process_text(&build,request->env_values[i]);
        if(build.status==XR_OS_IO_OK&&(!*texts[at+i*2]||strchr(texts[at+i*2],'=')))refused=true;
    }
    if(build.status!=XR_OS_IO_OK)goto done;
    if(!*texts[0]||refused){refused=true;goto done;}
    XrProcMemory text_memory={&build,owned_process_text_allocate,owned_process_free};
    ProcBuild text={.memory=text_memory,.status=XR_PROC_OK,.query_context=&build,.current=owned_process_current};
    if(shell)command=owned_process_shell_command(&text,texts[0],&program,&refused);
    else {command=proc_command(&text,(const char *const *)texts);program=proc_wide(&text,texts[0]);}
    if(refused)goto done;
    if(request->cwd&&text.status==XR_PROC_OK)cwd=proc_wide(&text,texts[request->argument_count+2]);
    if(text.status!=XR_PROC_OK){status=owned_process_text_status(text.status);goto done;}
    if(!proc_exact_program(program)) {
        DWORD needed=SearchPathW(NULL,program,L".exe",0,NULL,NULL);
        if(!needed){DWORD error=GetLastError();refused=owned_process_refusal(error);status=refused?XR_OS_IO_OK:owned_process_error(error);goto done;}
        wchar_t *resolved=proc_alloc(&text,(size_t)needed*sizeof(wchar_t));
        if(!resolved){status=owned_process_text_status(text.status);goto done;}
        DWORD length=SearchPathW(NULL,program,L".exe",needed,resolved,NULL);
        if(!length||length>=needed){DWORD error=GetLastError();proc_free(&text,resolved);refused=!length&&owned_process_refusal(error);status=refused?XR_OS_IO_OK:owned_process_error(error);goto done;}
        proc_free(&text,program);program=resolved;
    }
    state->inherited=GetEnvironmentStringsW();if(!state->inherited){status=owned_process_error(GetLastError());goto done;}
    text.inherited=state->inherited;
    const char **keys=NULL,**values=NULL;
    if(request->environment_count) {
        keys=proc_alloc(&text,request->environment_count*sizeof(*keys));
        values=proc_alloc(&text,request->environment_count*sizeof(*values));
        if(!keys||!values){proc_free(&text,keys);proc_free(&text,values);status=owned_process_text_status(text.status);goto done;}
        for(size_t i=0;i<request->environment_count;++i){keys[i]=texts[at+i*2];values[i]=texts[at+i*2+1];}
    }
    XrProcSpawnOptions options={.memory=text_memory,.env_keys=keys,.env_values=values,.env_count=request->environment_count};
    environment=proc_environment(&text,&options);proc_free(&text,keys);proc_free(&text,values);
    if(text.status!=XR_PROC_OK){status=owned_process_text_status(text.status);goto done;}
    if(!FreeEnvironmentStringsW(state->inherited)){status=owned_process_error(GetLastError());goto done;}
    state->inherited=NULL;
    for(uint32_t i=0;i<3;++i) {
        if(!owned_process_current(&build))goto done;
        HANDLE source=request->redirected[i]?(HANDLE)request->streams[i]:GetStdHandle(i==0?STD_INPUT_HANDLE:i==1?STD_OUTPUT_HANDLE:STD_ERROR_HANDLE);
        if(!source||source==INVALID_HANDLE_VALUE) {
            SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
            state->streams[i]=CreateFileW(L"NUL",i?GENERIC_WRITE:GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,NULL);
            if(state->streams[i]==INVALID_HANDLE_VALUE)state->streams[i]=NULL;
            if(!state->streams[i]){status=owned_process_error(GetLastError());goto done;}
        } else if(!DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),state->streams+i,0,TRUE,DUPLICATE_SAME_ACCESS)) {
            status=owned_process_error(GetLastError());goto done;
        }
    }
    SIZE_T attribute_bytes=0;
    (void)InitializeProcThreadAttributeList(NULL,1,0,&attribute_bytes);
    if(!attribute_bytes){status=XR_OS_IO_IO;goto done;}
    startup.lpAttributeList=proc_alloc(&text,attribute_bytes);
    if(!startup.lpAttributeList){status=owned_process_text_status(text.status);goto done;}
    if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&attribute_bytes)){status=owned_process_error(GetLastError());goto done;}
    attributes=true;
    if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,state->streams,sizeof(state->streams),NULL,NULL)) {
        status=owned_process_error(GetLastError());goto done;
    }
    state->job=CreateJobObjectW(NULL,NULL);if(!state->job){status=owned_process_error(GetLastError());goto done;}
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(state->job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){status=owned_process_error(GetLastError());goto done;}
    startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=state->streams[0];startup.StartupInfo.hStdOutput=state->streams[1];startup.StartupInfo.hStdError=state->streams[2];
    if(!owned_process_current(&build))goto done;
    PROCESS_INFORMATION child={0};
    if(!CreateProcessW(program,command,NULL,NULL,TRUE,CREATE_SUSPENDED|CREATE_UNICODE_ENVIRONMENT|
        EXTENDED_STARTUPINFO_PRESENT|(request->detached?CREATE_NEW_PROCESS_GROUP:0),environment,cwd,&startup.StartupInfo,&child)) {
        DWORD error=GetLastError();refused=owned_process_refusal(error);status=refused?XR_OS_IO_OK:owned_process_error(error);goto done;
    }
    state->process=child.hProcess;state->thread=child.hThread;
    if(!AssignProcessToJobObject(state->job,state->process)){status=owned_process_error(GetLastError());goto done;}
    state->assigned=true;
    for(uint32_t i=0;i<3;++i) {
        if(!CloseHandle(state->streams[i])){status=owned_process_error(GetLastError());goto done;}
        state->streams[i]=NULL;
    }
    if(!owned_process_current(&build))goto done;
    state->prepared=true;
done:
    if(attributes)DeleteProcThreadAttributeList(startup.lpAttributeList);
    if(startup.lpAttributeList)owned_process_free(&build,startup.lpAttributeList);
    owned_process_free(&build,environment);owned_process_free(&build,cwd);
    owned_process_free(&build,command);owned_process_free(&build,program);
    if(texts){for(size_t i=0;i<count;++i)owned_process_free(&build,texts[i]);owned_process_free(&build,texts);}
    if(build.status!=XR_OS_IO_OK)status=build.status;
    if(status==XR_OS_IO_OK)*created=!refused&&state&&state->prepared;
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_process_prepare(const XrOsIoPolicy *memory,
    const XrOsProcessQuery *query,const XrOsProcessRequest *request,XrOsProcessOwner *owner,bool *created) {
    return owned_process_prepare(memory,query,request,NULL,owner,created);
}
XR_FUNC XrOsIoStatus xr_os_io_process_prepare_shell(const XrOsIoPolicy *memory,
    const XrOsProcessQuery *query,const XrOsProcessRequest *request,
    const XrOsProcessText *command,XrOsProcessOwner *owner,bool *created) {
    if(!command)return XR_OS_IO_BAD_ARGUMENT;
    return owned_process_prepare(memory,query,request,command,owner,created);
}
XR_FUNC XrOsIoStatus xr_os_io_process_start(XrOsProcessOwner *owner) {
    OwnedProcessState *state=owned_process_state(owner);
    if(!state||owner->busy||!state->prepared||state->started||!state->process||!state->thread||!state->job)return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    if(state->detached) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
        if(!QueryInformationJobObject(state->job,JobObjectExtendedLimitInformation,&limits,sizeof(limits),NULL))status=owned_process_error(GetLastError());
        else {limits.BasicLimitInformation.LimitFlags&=~JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if(!SetInformationJobObject(state->job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))status=owned_process_error(GetLastError());}
    }
    if(status==XR_OS_IO_OK) {
        if(ResumeThread(state->thread)==(DWORD)-1)status=owned_process_error(GetLastError());
        else {state->started=true;state->independent=state->detached;}
    }
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_process_poll(XrOsProcessOwner *owner,XrOsProcessProgress *output) {
    OwnedProcessState *state=owned_process_state(owner);
    if(!state||owner->busy||!output||!state->started||state->independent||state->terminating)return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;XrOsProcessProgress progress={0};
    DWORD wait=WaitForSingleObject(state->process,0),code=0;
    if(wait==WAIT_OBJECT_0) {
        if(!GetExitCodeProcess(state->process,&code))status=owned_process_error(GetLastError());
        else {progress.exited=true;progress.code=code==OWNED_PROCESS_KILLED?-1:
            code<=INT32_MAX?(int64_t)code:(int64_t)code-INT64_C(4294967296);}
    } else if(wait!=WAIT_TIMEOUT)status=owned_process_error(GetLastError());
    if(status==XR_OS_IO_OK)*output=progress;
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_process_kill(XrOsProcessOwner *owner,int64_t signal,bool *sent) {
    OwnedProcessState *state=owned_process_state(owner);
    if(!state||owner->busy||!sent||!state->started||state->independent||state->terminating)return XR_OS_IO_BAD_ARGUMENT;
    if(signal<=0||signal>INT_MAX){*sent=false;return XR_OS_IO_OK;}
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    if(TerminateProcess(state->process,OWNED_PROCESS_KILLED))*sent=true;
    else {DWORD error=GetLastError();if(error==ERROR_ACCESS_DENIED)*sent=false;else status=owned_process_error(error);}
    owner->busy=false;return status;
}
XR_FUNC XrOsIoStatus xr_os_io_process_close(XrOsProcessOwner *owner,bool *closed) {
    if(!owner||owner->busy||!closed)return XR_OS_IO_BAD_ARGUMENT;
    if(!owner->state){*closed=true;return XR_OS_IO_OK;}
    OwnedProcessState *state=owned_process_state(owner);if(!state)return XR_OS_IO_BAD_ARGUMENT;
    owner->busy=true;XrOsIoStatus status=XR_OS_IO_OK;
    if(!state->independent&&state->process) {
        if(!state->terminating) {
            if(state->assigned&&!TerminateJobObject(state->job,OWNED_PROCESS_KILLED)) {
                status=owned_process_error(GetLastError());goto done;
            }
            if(!state->assigned&&WaitForSingleObject(state->process,0)!=WAIT_OBJECT_0&&!TerminateProcess(state->process,OWNED_PROCESS_KILLED)) {
                status=owned_process_error(GetLastError());goto done;
            }
            state->terminating=true;
        }
        DWORD wait=WaitForSingleObject(state->process,0);
        if(wait==WAIT_TIMEOUT){*closed=false;goto done;}
        if(wait!=WAIT_OBJECT_0){status=owned_process_error(GetLastError());goto done;}
        if(state->job) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting={0};
            if(!QueryInformationJobObject(state->job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),NULL)) {
                status=owned_process_error(GetLastError());goto done;
            }
            if(accounting.ActiveProcesses){*closed=false;goto done;}
        }
    }
    if(state->inherited){if(!FreeEnvironmentStringsW(state->inherited)){status=owned_process_error(GetLastError());goto done;}state->inherited=NULL;}
    HANDLE *handles[]={&state->thread,&state->process,&state->job,&state->streams[0],&state->streams[1],&state->streams[2]};
    for(uint32_t i=0;i<sizeof(handles)/sizeof(*handles);++i)if(*handles[i]) {
        if(!CloseHandle(*handles[i])){status=owned_process_error(GetLastError());goto done;}*handles[i]=NULL;
    }
    XrOsIoPolicy memory=state->memory;owner->state=NULL;memory.free(memory.context,state);*closed=true;
done:
    owner->busy=false;return status;
}
