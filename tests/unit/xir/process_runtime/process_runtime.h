/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * process_runtime.h - Real children observed through Process and public waits
 *
 * KEY CONCEPT:
 *   Child cooperation exposes only OS-visible results. Runtime tokens never
 *   become native PIDs or handles; test cleanup owns only its reported children.
 */
#ifndef PROCESS_RUNTIME_H
#define PROCESS_RUNTIME_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_native_callable.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_time.h"
#include "os/os_thread.h"
#include "os/os_process_owned.h"
#include "os/os_pipe_owned.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#ifdef interface
#undef interface
#endif
#include <io.h>
#include <fcntl.h>
#endif
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"Process product %d: %s\n",__LINE__,#c);exit(1); } } while(0)
#if defined(XR_OS_WINDOWS)
static XrOsIoStatus process_observed_prepare(const XrOsIoPolicy *,const XrOsProcessQuery *,const XrOsProcessRequest *,XrOsProcessOwner *,bool *);
static XrOsIoStatus process_observed_prepare_shell(const XrOsIoPolicy *,const XrOsProcessQuery *,const XrOsProcessRequest *,const XrOsProcessText *,XrOsProcessOwner *,bool *);
static XrOsIoStatus process_observed_start(XrOsProcessOwner *);
static XrOsIoStatus process_complete_os_begin(XrOsPipeOwner *,const XrOsPipeRequest *,const XrOsPipeQuery *,XrOsPipeOperation *);
static XrOsIoStatus process_complete_os_poll(XrOsPipeOperation *,XrOsPipeProgress *);
static XrOsIoStatus process_complete_os_operation_close(XrOsPipeOperation *,bool *);
static XrOsIoStatus process_complete_os_close_end(XrOsPipeOwner *,XrOsPipeEnd,bool *);
static XrOsIoStatus process_complete_os_dispose(XrOsPipeOwner *,bool *);
#define xr_os_io_process_prepare process_observed_prepare
#define xr_os_io_process_prepare_shell process_observed_prepare_shell
#define xr_os_io_process_start process_observed_start
#define xr_os_io_pipe_begin process_complete_os_begin
#define xr_os_io_pipe_poll process_complete_os_poll
#define xr_os_io_pipe_operation_close process_complete_os_operation_close
#define xr_os_io_pipe_close_end process_complete_os_close_end
#define xr_os_io_pipe_dispose process_complete_os_dispose
#endif
#include "../xir_runtime_allocations.h"
#if defined(XR_OS_WINDOWS)
#undef xr_os_io_process_prepare
#undef xr_os_io_process_prepare_shell
#undef xr_os_io_process_start
#undef xr_os_io_pipe_begin
#undef xr_os_io_pipe_poll
#undef xr_os_io_pipe_operation_close
#undef xr_os_io_pipe_close_end
#undef xr_os_io_pipe_dispose
#endif
#include "process_cases.h"

static void process_trace(const char *text) {
    const char *enabled=getenv("XR_PROCESS_TRACE");
    if(enabled&&!strcmp(enabled,"1"))fprintf(stderr,"Process trace: %s\n",text);
}
static void process_resume(XrXirInstance *instance,XrXirInstanceResult event) {
    XrXirWaitRequest request={0};
    CHECK(event.epoch&&xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
    if(request.kind==XR_XIR_WAIT_TIMER_MS) {
        CHECK(request.after_ms<=XR_XIR_TIMER_MAX_MS);xr_time_sleep_ms(request.after_ms);
    } else CHECK(request.kind==XR_XIR_WAIT_YIELD);
    CHECK(xr_xir_instance_resume(instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
}
static XrXirInstanceResult process_drive(XrXirInstance *instance) {
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;) {
        XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED)process_resume(instance,event);
        else if(event.outcome.status!=XR_XIR_CALL_READY)return event;
        uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(10000000000));
    }
}
static XrXirValue process_invoke(XrXirInstance *instance,uint32_t entry,const XrXirValue *args,uint32_t count) {
    XrXirCallStatus status=xr_xir_instance_start(instance,entry,args,count);
    if(status!=XR_XIR_CALL_READY)fprintf(stderr,"Process start entry%u status%u\n",entry,status);
    CHECK(status==XR_XIR_CALL_READY);XrXirInstanceResult event=process_drive(instance);
    if(event.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"Process entry%u status%u\n",entry,event.outcome.status);
    CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void process_retire(XrXirInstance *instance) {
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;) {
        XrXirCallStatus status=xr_xir_instance_free(instance);
        if(status==XR_XIR_CALL_READY)return;
        CHECK(status==XR_XIR_CALL_CLOSE_FAILED);uint64_t now=xr_time_monotonic_ns();
        CHECK(now>=started&&now-started<UINT64_C(10000000000));xr_thread_yield();
    }
}
static XrXirInstance *process_instance(XrXirProgram *program,const uint32_t *ids) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue value=process_invoke(instance,ids[PROCESS_ENTRY],NULL,0);
    CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);return instance;
}
static const XrXirValue *process_some(const XrXirValue *optional,bool expected) {
    bool present=false;const XrXirValue *value=NULL;
    CHECK(xr_xir_nullable_view(optional,&present,&value)&&present==expected);
    CHECK(expected?value!=NULL:value==NULL);return value;
}
static XrXirValue process_owned_some(XrXirValue *optional) {
    XrXirValue value={0};CHECK(xr_xir_value_copy(process_some(optional,true),&value)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(optional);return value;
}
static void process_bool(XrXirInstance *instance,uint32_t entry,const XrXirValue *value,bool expected) {
    XrXirValue result=process_invoke(instance,entry,value,1);
    CHECK(result.type==XR_XIR_BOOL&&result.payload==expected);xr_xir_value_drop(&result);
}
static XrXirValueAdmission process_access(const XrXirValue *value) {
    CHECK(xr_xir_value_valid(value));
    return (XrXirValueAdmission){.arena=xr_xir_value_arena(value),.domain=object_pointer(value)->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
}

#if defined(XR_OS_WINDOWS)
typedef struct ProcessChildRecord { DWORD pid[2];wchar_t cwd[32768]; } ProcessChildRecord;
typedef struct ProcessChild {
    HANDLE mapping,ready,release,leaf_ready,leaf_release,leader,leaf;
    ProcessChildRecord *record;
    char name[128];
} ProcessChild;
static unsigned process_child_serial;
static void process_object_name(const char *base,const char *suffix,char output[160]) {
    int count=snprintf(output,160,"%s-%s",base,suffix);CHECK(count>0&&count<160);
}
static HANDLE process_event(const char *base,const char *suffix,bool create) {
    char name[160];process_object_name(base,suffix,name);
    HANDLE event=create?CreateEventA(NULL,TRUE,FALSE,name):OpenEventA(SYNCHRONIZE|EVENT_MODIFY_STATE,FALSE,name);
    CHECK(event);return event;
}
static void process_child_open(ProcessChild *child) {
    memset(child,0,sizeof(*child));
    int count=snprintf(child->name,sizeof(child->name),"Local\\xray-process-%lu-%u",(unsigned long)GetCurrentProcessId(),++process_child_serial);
    CHECK(count>0&&(size_t)count<sizeof(child->name));char name[160];process_object_name(child->name,"map",name);
    child->mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,NULL,PAGE_READWRITE,0,(DWORD)sizeof(ProcessChildRecord),name);
    CHECK(child->mapping&&GetLastError()!=ERROR_ALREADY_EXISTS);
    child->record=MapViewOfFile(child->mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(ProcessChildRecord));CHECK(child->record);
    memset(child->record,0,sizeof(*child->record));
    child->ready=process_event(child->name,"ready",true);child->release=process_event(child->name,"release",true);
    child->leaf_ready=process_event(child->name,"leaf-ready",true);child->leaf_release=process_event(child->name,"leaf-release",true);
}
static void process_child_observe(ProcessChild *child,bool tree) {
    CHECK(WaitForSingleObject(child->ready,10000)==WAIT_OBJECT_0&&child->record->pid[0]);
    child->leader=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,child->record->pid[0]);CHECK(child->leader);
    if(tree) {
        CHECK(child->record->pid[1]);child->leaf=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,child->record->pid[1]);CHECK(child->leaf);
    }
}
static ProcessChild *process_oom_child;
static bool process_oom_observing,process_oom_armed;
static unsigned process_oom_prepares,process_oom_starts,process_oom_hits;
static size_t process_oom_allocation;
static XrOsIoStatus process_observed_prepare(const XrOsIoPolicy *memory,const XrOsProcessQuery *query,
    const XrOsProcessRequest *request,XrOsProcessOwner *owner,bool *created) {
    if(process_oom_observing)++process_oom_prepares;
    return xr_os_io_process_prepare(memory,query,request,owner,created);
}
static XrOsIoStatus process_observed_prepare_shell(const XrOsIoPolicy *memory,const XrOsProcessQuery *query,
    const XrOsProcessRequest *request,const XrOsProcessText *command,XrOsProcessOwner *owner,bool *created){
    if(process_oom_observing)++process_oom_prepares;
    return xr_os_io_process_prepare_shell(memory,query,request,command,owner,created);
}
static XrOsIoStatus process_observed_start(XrOsProcessOwner *owner) {
    if(process_oom_observing)++process_oom_starts;
    XrOsIoStatus status=xr_os_io_process_start(owner);
    if(process_oom_observing&&process_oom_armed&&status==XR_OS_IO_OK) {
        /* The native start has succeeded. Only the subsequent real public
         * Process result path is faulted; no guest or OS call is faked. */
        CHECK(process_oom_child);process_child_observe(process_oom_child,false);
        ++process_oom_hits;process_oom_armed=false;process_oom_allocation=runtime_attempts;
        runtime_fail_at=process_oom_allocation;
    }
    return status;
}
static void process_child_done(HANDLE child,DWORD expected,bool exact) {
    CHECK(WaitForSingleObject(child,10000)==WAIT_OBJECT_0);DWORD code=STILL_ACTIVE;
    CHECK(GetExitCodeProcess(child,&code)&&code!=STILL_ACTIVE);if(exact)CHECK(code==expected);
}
static void process_child_close(ProcessChild *child) {
    HANDLE handles[]={child->leader,child->leaf,child->ready,child->release,child->leaf_ready,child->leaf_release};
    for(unsigned n=0;n<sizeof(handles)/sizeof(*handles);++n)if(handles[n])CHECK(CloseHandle(handles[n]));
    CHECK(UnmapViewOfFile(child->record)&&CloseHandle(child->mapping));memset(child,0,sizeof(*child));
}
static void process_child_bytes(HANDLE handle,const unsigned char *bytes,size_t length,bool write) {
    size_t offset=0;unsigned char actual[sizeof(process_input)]={0};CHECK(length<=sizeof(actual));
    while(offset<length) {
        DWORD count=0;BOOL ok=write?WriteFile(handle,bytes+offset,(DWORD)(length-offset),&count,NULL):ReadFile(handle,actual+offset,(DWORD)(length-offset),&count,NULL);
        CHECK(ok&&count&&count<=length-offset);offset+=count;
    }
    if(!write)CHECK(!memcmp(actual,bytes,length));
}
static void process_child_tree(const char *base,ProcessChildRecord *record) {
    wchar_t executable[32768],command[33000];DWORD count=GetModuleFileNameW(NULL,executable,32768);CHECK(count&&count<32768);
    wchar_t name[128];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,base,-1,name,128)>0);
    CHECK(swprintf(command,33000,L"\"%ls\" --process-child leaf %ls",executable,name)>0);
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION info={0};
    CHECK(CreateProcessW(executable,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&info));record->pid[1]=info.dwProcessId;
    HANDLE ready=process_event(base,"leaf-ready",false);CHECK(WaitForSingleObject(ready,10000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(ready)&&CloseHandle(info.hThread)&&CloseHandle(info.hProcess));
}
static bool process_child(int argc,char **argv) {
    char inherited[128]={0},name[160];HANDLE mapping=NULL;const char *mode=NULL,*base=NULL;
    if(argc==1) {
        static const char prefix[]="Local\\xray-process-";
        DWORD length=GetEnvironmentVariableA("XR_PROCESS_NOARGS_CHILD",inherited,sizeof(inherited));
        if(!length||length>=sizeof(inherited)||strncmp(inherited,prefix,sizeof(prefix)-1))return false;
        process_object_name(inherited,"map",name);mapping=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name);
        if(!mapping)return false;mode="hold";base=inherited;
    } else {
        if(strcmp(argv[1],"--process-child"))return false;
        CHECK(argc==4);mode=argv[2];base=argv[3];process_object_name(base,"map",name);
        mapping=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,name);CHECK(mapping);
    }
    ProcessChildRecord *record=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(ProcessChildRecord));CHECK(record);
    bool leaf=!strcmp(mode,"leaf");record->pid[leaf?1:0]=GetCurrentProcessId();
    HANDLE ready=process_event(base,leaf?"leaf-ready":"ready",false),release=process_event(base,leaf?"leaf-release":"release",false);
    if(!strcmp(mode,"tree"))process_child_tree(base,record);
    if(!strcmp(mode,"configured")) {
        char environment[32];CHECK(GetEnvironmentVariableA("XR_PROCESS_CHILD",environment,sizeof(environment))==9&&!strcmp(environment,"exact-env"));
        wchar_t directory[32768];DWORD got=GetCurrentDirectoryW(32768,directory);
        CHECK(got&&got<32768&&record->cwd[0]&&!_wcsicmp(directory,record->cwd));
    }
    CHECK(SetEvent(ready));
    if(!strcmp(mode,"stdio")) {
        process_child_bytes(GetStdHandle(STD_INPUT_HANDLE),process_input,sizeof(process_input),false);
        process_child_bytes(GetStdHandle(STD_OUTPUT_HANDLE),process_input,sizeof(process_input),true);
        process_child_bytes(GetStdHandle(STD_ERROR_HANDLE),process_error,sizeof(process_error),true);
    } else CHECK(WaitForSingleObject(release,10000)==WAIT_OBJECT_0);
    CHECK(CloseHandle(ready)&&CloseHandle(release)&&UnmapViewOfFile(record)&&CloseHandle(mapping));
    ExitProcess(!strcmp(mode,"zero")?0u:23u);
}
static char *process_utf8(const wchar_t *wide) {
    int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,NULL,0,NULL,NULL);CHECK(count>0);
    char *text=xr_malloc((size_t)count);CHECK(text&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,text,count,NULL,NULL)==count);return text;
}
static XrXirValue process_string(XrXirInstance *instance,const char *text) {
    XrXirValue value={0};CHECK(xr_xir_string_new(instance->domain,text,strlen(text),&value)==XR_XIR_VALUE_OK);return value;
}
static XrXirValue process_spawn(XrXirInstance *instance,XrXirProgram *program,const uint32_t *ids,ProcessChild *child,const char *mode,uint32_t entry) {
    wchar_t path[32768];DWORD count=GetModuleFileNameW(NULL,path,32768);CHECK(count&&count<32768);char *text=process_utf8(path);
    XrXirValue executable=process_string(instance,text);xr_free(text);
    XrXirValue cwd={0};
    if(entry==PROCESS_CONFIGURED) {
        wchar_t directory[32768];DWORD length=GetTempPathW(32768,directory);CHECK(length&&length<32768);
        text=process_utf8(directory);cwd=process_string(instance,text);xr_free(text);
        while(length>3&&(directory[length-1]==L'\\'||directory[length-1]==L'/'))directory[--length]=0;
        memcpy(child->record->cwd,directory,((size_t)length+1)*sizeof(wchar_t));
    }
    XrXirValue values[]={process_string(instance,"--process-child"),process_string(instance,mode),process_string(instance,child->name)},arguments={0};
    XrXirValueAdmission access={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
    bool noargs=!strcmp(mode,"noargs");
    CHECK(xr_xir_array_new(program->entries[ids[PROCESS_SPAWN]].parameters[1],values,noargs?0u:3u,&access,&arguments)==XR_XIR_VALUE_OK);
    for(unsigned n=0;n<3;++n)xr_xir_value_drop(&values[n]);
    XrXirValue args[]={executable,arguments,cwd},optional={0};
    wchar_t *previous=NULL;
    if(noargs) {
        /* Preserve the exact process-local value, including absence and UTF-16. */
        SetLastError(ERROR_SUCCESS);DWORD required=GetEnvironmentVariableW(L"XR_PROCESS_NOARGS_CHILD",NULL,0),error=GetLastError();
        if(required||error==ERROR_SUCCESS) {
            size_t units=required?required:1;CHECK(units<=SIZE_MAX/sizeof(wchar_t));previous=xr_malloc(units*sizeof(wchar_t));CHECK(previous);
            if(required)CHECK(GetEnvironmentVariableW(L"XR_PROCESS_NOARGS_CHILD",previous,required)==required-1);else previous[0]=0;
        } else CHECK(error==ERROR_ENVVAR_NOT_FOUND);
        wchar_t marker[128];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,child->name,-1,marker,128)>0);
        CHECK(SetEnvironmentVariableW(L"XR_PROCESS_NOARGS_CHILD",marker));
    }
    optional=process_invoke(instance,ids[entry],args,entry==PROCESS_CONFIGURED?3u:2u);
    if(noargs) { CHECK(SetEnvironmentVariableW(L"XR_PROCESS_NOARGS_CHILD",previous));xr_free(previous); }
    for(unsigned n=0;n<3;++n)xr_xir_value_drop(&args[n]);return process_owned_some(&optional);
}
static void process_code(XrXirInstance *instance,uint32_t entry,const XrXirValue *process,int64_t expected) {
    XrXirValue result=process_invoke(instance,entry,process,1);CHECK(result.type==XR_XIR_I64&&result.payload==expected);xr_xir_value_drop(&result);
}
static void process_basic(XrXirProgram *program,const uint32_t *ids,uint32_t entry,const char *mode) {
    XrXirInstance *instance=process_instance(program,ids);ProcessChild child;process_child_open(&child);
    XrXirValue process=process_spawn(instance,program,ids,&child,mode,entry);process_child_observe(&child,false);
    XrXirValue token=process_invoke(instance,ids[PROCESS_ID],&process,1);CHECK(token.type==XR_XIR_I64&&token.payload>0);
    XrXirValue pending=process_invoke(instance,ids[PROCESS_TRY_WAIT],&process,1);process_some(&pending,false);xr_xir_value_drop(&pending);
    CHECK(SetEvent(child.release));process_child_done(child.leader,!strcmp(mode,"zero")?0u:23u,true);
    XrXirValue code=process_invoke(instance,ids[PROCESS_TRY_WAIT],&process,1);
    CHECK(process_some(&code,true)->type==XR_XIR_I64&&process_some(&code,true)->payload==(!strcmp(mode,"zero")?0:23));
    process_code(instance,ids[PROCESS_WAIT],&process,-1);xr_xir_value_drop(&process);xr_xir_value_drop(&token);
    process_retire(instance);CHECK(process_some(&code,true)->payload==(!strcmp(mode,"zero")?0:23));xr_xir_value_drop(&code);process_child_close(&child);
}
static void process_wait_resume(XrXirProgram *program,const uint32_t *ids,const char *mode) {
    size_t live=runtime_live,bytes=runtime_bytes;
    XrXirInstance *instance=process_instance(program,ids);ProcessChild child;process_child_open(&child);
    XrXirValue process=process_spawn(instance,program,ids,&child,mode,PROCESS_SPAWN);process_child_observe(&child,false);
    CHECK(xr_xir_instance_start(instance,ids[PROCESS_WAIT],&process,1)==XR_XIR_CALL_READY);
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;) {
        XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED) {
            XrXirWaitRequest request={0};CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
            CHECK(request.kind==XR_XIR_WAIT_TIMER_MS&&request.after_ms>0&&SetEvent(child.release));
            process_resume(instance,event);break;
        }
        CHECK(event.outcome.status==XR_XIR_CALL_READY);uint64_t now=xr_time_monotonic_ns();
        CHECK(now>=started&&now-started<UINT64_C(10000000000));
    }
    CHECK(process_drive(instance).outcome.status==XR_XIR_CALL_RETURNED);XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED&&result.type==XR_XIR_I64&&result.payload==23);
    process_child_done(child.leader,23,true);xr_xir_value_drop(&result);xr_xir_value_drop(&process);
    process_retire(instance);process_child_close(&child);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void process_kill_and_retire(XrXirProgram *program,const uint32_t *ids,bool kill) {
    XrXirInstance *instance=process_instance(program,ids);ProcessChild child;process_child_open(&child);
    XrXirValue process=process_spawn(instance,program,ids,&child,"hold",PROCESS_SPAWN);process_child_observe(&child,false);
    if(kill) {
        process_bool(instance,ids[PROCESS_KILL],&process,true);process_child_done(child.leader,0,false);
        DWORD actual=0;CHECK(GetExitCodeProcess(child.leader,&actual)&&actual!=STILL_ACTIVE);
        /* Portable forced termination is -1, rather than the raw Win32 code. */
        CHECK(actual==UINT32_C(0xe0000001));process_code(instance,ids[PROCESS_WAIT],&process,-1);
    }
    xr_xir_value_drop(&process);process_retire(instance);process_child_done(child.leader,0,false);process_child_close(&child);
}
static void process_detached_and_tree(XrXirProgram *program,const uint32_t *ids,bool detached) {
    XrXirInstance *instance=process_instance(program,ids);ProcessChild child;process_child_open(&child);
    XrXirValue process=process_spawn(instance,program,ids,&child,detached?"hold":"tree",detached?PROCESS_DETACHED:PROCESS_SPAWN);
    process_child_observe(&child,!detached);
    if(detached) {
        process_code(instance,ids[PROCESS_WAIT],&process,-1);xr_xir_value_drop(&process);process_retire(instance);
        CHECK(WaitForSingleObject(child.leader,0)==WAIT_TIMEOUT&&SetEvent(child.release));process_child_done(child.leader,23,true);
    } else {
        CHECK(SetEvent(child.release));process_child_done(child.leader,23,true);
        /* tryWait observes only the leader; the live descendant is Root-owned. */
        XrXirValue code=process_invoke(instance,ids[PROCESS_TRY_WAIT],&process,1);
        CHECK(process_some(&code,true)->type==XR_XIR_I64&&process_some(&code,true)->payload==23);xr_xir_value_drop(&code);
        CHECK(WaitForSingleObject(child.leaf,0)==WAIT_TIMEOUT);xr_xir_value_drop(&process);process_retire(instance);process_child_done(child.leaf,0,false);
    }
    process_child_close(&child);
}
static void process_refusals(XrXirProgram *program,const uint32_t *ids) {
    XrXirInstance *a=process_instance(program,ids),*b=process_instance(program,ids);
    ProcessChild child;process_child_open(&child);XrXirValue process=process_spawn(a,program,ids,&child,"hold",PROCESS_SPAWN);
    process_child_observe(&child,false);XrXirValue token=process_invoke(a,ids[PROCESS_ID],&process,1);
    CHECK(token.type==XR_XIR_I64&&token.payload>0);
    XrXirValue rejected[]={token,{XR_XIR_I64,0,0},{XR_XIR_I64,0,INT64_MAX}};
    for(unsigned n=0;n<3;++n) {
        XrXirValue result=process_invoke(b,ids[PROCESS_RAW_WAIT],&rejected[n],1);
        CHECK(process_some(&result,true)->type==XR_XIR_I64&&process_some(&result,true)->payload==-1);xr_xir_value_drop(&result);
        process_bool(b,ids[PROCESS_RAW_KILL],&rejected[n],false);
    }
    CHECK(WaitForSingleObject(child.leader,0)==WAIT_TIMEOUT);xr_xir_value_drop(&process);process_retire(a);process_child_done(child.leader,0,false);
    XrXirInstance *fresh=process_instance(program,ids);XrXirValue stale=process_invoke(fresh,ids[PROCESS_RAW_WAIT],&token,1);
    CHECK(process_some(&stale,true)->payload==-1);xr_xir_value_drop(&stale);process_bool(fresh,ids[PROCESS_RAW_KILL],&token,false);
    ProcessChild next;process_child_open(&next);XrXirValue replacement=process_spawn(fresh,program,ids,&next,"hold",PROCESS_SPAWN);
    process_child_observe(&next,false);XrXirValue next_token=process_invoke(fresh,ids[PROCESS_ID],&replacement,1);
    CHECK(next_token.type==XR_XIR_I64&&next_token.payload>0&&next_token.payload!=token.payload);
    xr_xir_value_drop(&next_token);xr_xir_value_drop(&replacement);xr_xir_value_drop(&token);
    process_retire(fresh);process_child_done(next.leader,0,false);process_child_close(&next);process_retire(b);process_child_close(&child);
}
static XrXirValue process_pipe_open(XrXirInstance *instance,const uint32_t *ids) {
    XrXirValue optional=process_invoke(instance,ids[PROCESS_PIPE_OPEN],NULL,0);return process_owned_some(&optional);
}
static XrXirValue process_spawn_streams(XrXirInstance *instance,XrXirProgram *program,const uint32_t *ids,ProcessChild *child,const XrXirValue pipes[3]) {
    wchar_t path[32768];DWORD count=GetModuleFileNameW(NULL,path,32768);CHECK(count&&count<32768);char *text=process_utf8(path);
    XrXirValue executable=process_string(instance,text);xr_free(text);
    XrXirValue elements[]={process_string(instance,"--process-child"),process_string(instance,"stdio"),process_string(instance,child->name)},arguments={0};
    XrXirValueAdmission access={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
    CHECK(xr_xir_array_new(program->entries[ids[PROCESS_SPAWN]].parameters[1],elements,3,&access,&arguments)==XR_XIR_VALUE_OK);
    for(unsigned n=0;n<3;++n)xr_xir_value_drop(&elements[n]);
    XrXirValue args[]={executable,arguments,pipes[0],pipes[1],pipes[2]},optional=process_invoke(instance,ids[PROCESS_STREAMS],args,5);
    xr_xir_value_drop(&executable);xr_xir_value_drop(&arguments);return process_owned_some(&optional);
}
static void process_read_bytes(XrXirInstance *instance,const uint32_t *ids,const XrXirValue *pipe,const unsigned char *bytes,size_t length) {
    size_t offset=0;
    for(;;) {
        XrXirValue args[]={*pipe,{XR_XIR_I64,0,5}},optional=process_invoke(instance,ids[PROCESS_PIPE_READ],args,2);
        const XrXirValue *array=process_some(&optional,true);XrXirValueAdmission access=process_access(array);int64_t count=-1;
        CHECK(xr_xir_array_len(array,&access,&count)==XR_XIR_VALUE_OK&&count>=0&&(uint64_t)count<=length-offset);
        for(int64_t n=0;n<count;++n) {
            XrXirValue byte={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(array,n,&access,&byte,&fault)==XR_XIR_VALUE_OK&&byte.type==XR_XIR_U8&&byte.payload==bytes[offset+(size_t)n]);
            xr_xir_value_drop(&byte);
        }
        offset+=(size_t)count;xr_xir_value_drop(&optional);if(!count)break;
    }
    CHECK(offset==length);
}
static void process_streams(XrXirProgram *program,const uint32_t *ids) {
    XrXirInstance *instance=process_instance(program,ids);ProcessChild child;process_child_open(&child);
    XrXirValue pipes[]={process_pipe_open(instance,ids),process_pipe_open(instance,ids),process_pipe_open(instance,ids)};
    XrXirValue process=process_spawn_streams(instance,program,ids,&child,pipes);process_child_observe(&child,false);
    XrXirValue elements[sizeof(process_input)]={0},data={0};
    for(unsigned n=0;n<sizeof(process_input);++n)elements[n]=(XrXirValue){XR_XIR_U8,0,process_input[n]};
    XrXirValueAdmission access={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
    CHECK(xr_xir_array_new(program->entries[ids[PROCESS_PIPE_WRITE]].parameters[1],elements,sizeof(process_input),&access,&data)==XR_XIR_VALUE_OK);
    XrXirValue args[]={pipes[0],data},written=process_invoke(instance,ids[PROCESS_PIPE_WRITE],args,2);
    CHECK(written.type==XR_XIR_I64&&written.payload==(int64_t)sizeof(process_input));xr_xir_value_drop(&written);
    /* Inheritable copies belong to the child. These original endpoints remain
     * owned and closeable by their original Pipe instances. */
    process_bool(instance,ids[PROCESS_PIPE_CLOSE_READ],&pipes[0],true);
    process_bool(instance,ids[PROCESS_PIPE_CLOSE_WRITE],&pipes[0],true);
    process_bool(instance,ids[PROCESS_PIPE_CLOSE_WRITE],&pipes[1],true);
    process_bool(instance,ids[PROCESS_PIPE_CLOSE_WRITE],&pipes[2],true);
    process_read_bytes(instance,ids,&pipes[1],process_input,sizeof(process_input));process_read_bytes(instance,ids,&pipes[2],process_error,sizeof(process_error));
    process_child_done(child.leader,23,true);process_code(instance,ids[PROCESS_WAIT],&process,23);
    access=process_access(&data);int64_t length=-1;CHECK(xr_xir_array_len(&data,&access,&length)==XR_XIR_VALUE_OK&&length==(int64_t)sizeof(process_input));
    for(unsigned n=0;n<sizeof(process_input);++n) {
        XrXirValue byte={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(&data,n,&access,&byte,&fault)==XR_XIR_VALUE_OK&&byte.payload==process_input[n]);xr_xir_value_drop(&byte);
    }
    for(unsigned n=0;n<3;++n){process_bool(instance,ids[PROCESS_PIPE_CLOSE],&pipes[n],true);xr_xir_value_drop(&pipes[n]);}
    xr_xir_value_drop(&data);xr_xir_value_drop(&process);process_retire(instance);process_child_close(&child);
}
static void process_cancel(XrXirProgram *program,const uint32_t *ids) {
    XrXirInstance *instance=process_instance(program,ids);ProcessChild child;process_child_open(&child);
    XrXirValue process=process_spawn(instance,program,ids,&child,"hold",PROCESS_SPAWN);process_child_observe(&child,false);
    CHECK(xr_xir_instance_start(instance,ids[PROCESS_WAIT],&process,1)==XR_XIR_CALL_READY);bool suspended=false;
    uint64_t started=xr_time_monotonic_ns();CHECK(started);
    while(!suspended) {
        XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
        if(event.outcome.status==XR_XIR_CALL_SUSPENDED) {
            XrXirWaitRequest request={0};CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&request)==XR_XIR_CALL_READY);
            CHECK(request.kind==XR_XIR_WAIT_TIMER_MS);suspended=true;
        } else CHECK(event.outcome.status==XR_XIR_CALL_READY);
        uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(10000000000));
    }
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(process_drive(instance).outcome.status==XR_XIR_CALL_CANCELLED);XrXirValue output={0};
    CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_BAD_STATE&&!output.type&&!output.reserved&&!output.payload);
    xr_xir_value_drop(&process);process_retire(instance);process_child_done(child.leader,0,false);process_child_close(&child);
}
static void process_private_host(XrXirProgram *program,const uint32_t *ids) {
    XrXirInstance *instance=process_instance(program,ids);XrXirValue args[9]={{0}};
    uint32_t native[3]={UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for(uint32_t f=0;f<program->entry_count;++f) {
        uint32_t id=program->declarations->functions[f].native_callable;if(id>=101&&id<=103)native[id-101]=f;
    }
    for(unsigned n=0;n<3;++n)CHECK(native[n]!=UINT32_MAX);
    args[0]=process_string(instance,"");XrXirValueAdmission access={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
    CHECK(xr_xir_array_new(program->entries[native[0]].parameters[1],NULL,0,&access,&args[1])==XR_XIR_VALUE_OK);
    for(unsigned n=2;n<8;++n)CHECK(xr_xir_nullable_new(program->entries[native[0]].parameters[n],NULL,&access,&args[n])==XR_XIR_VALUE_OK);
    args[8]=(XrXirValue){XR_XIR_BOOL,0,0};CHECK(xr_xir_instance_start(instance,native[0],args,9)==XR_XIR_CALL_BAD_ARGUMENT);
    for(unsigned n=0;n<9;++n)xr_xir_value_drop(&args[n]);
    args[0]=(XrXirValue){XR_XIR_I64,0,1};args[1]=(XrXirValue){XR_XIR_I64,0,15};
    CHECK(xr_xir_instance_start(instance,native[1],args,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(instance,native[2],args,2)==XR_XIR_CALL_BAD_ARGUMENT);process_retire(instance);
}
static void process_result_oom(XrXirProgram *program,const uint32_t *ids) {
    size_t live=runtime_live,bytes=runtime_bytes;CHECK(runtime_fail_at==SIZE_MAX&&!process_oom_observing&&!process_oom_armed);
    XrXirInstance *instance=process_instance(program,ids);ProcessChild child;process_child_open(&child);
    wchar_t path[32768];DWORD count=GetModuleFileNameW(NULL,path,32768);CHECK(count&&count<32768);char *text=process_utf8(path);
    XrXirValue executable=process_string(instance,text);xr_free(text);
    XrXirValue values[]={process_string(instance,"--process-child"),process_string(instance,"hold"),process_string(instance,child.name)},arguments={0};
    XrXirValueAdmission access={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
    CHECK(xr_xir_array_new(program->entries[ids[PROCESS_SPAWN]].parameters[1],values,3,&access,&arguments)==XR_XIR_VALUE_OK);
    for(unsigned n=0;n<3;++n)xr_xir_value_drop(&values[n]);
    XrXirValue args[]={executable,arguments};process_oom_prepares=process_oom_starts=process_oom_hits=0;
    process_oom_child=&child;process_oom_observing=process_oom_armed=true;
    CHECK(xr_xir_instance_start(instance,ids[PROCESS_SPAWN],args,2)==XR_XIR_CALL_READY);
    XrXirCallStatus terminal=process_drive(instance).outcome.status;runtime_fail_at=SIZE_MAX;
    if(terminal!=XR_XIR_CALL_OOM)fprintf(stderr,"Process post-start OOM status%u\n",terminal);
    CHECK(terminal==XR_XIR_CALL_OOM&&process_oom_hits==1&&!process_oom_armed&&child.leader&&runtime_attempts>process_oom_allocation);
    XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_OOM&&!output.type&&!output.reserved&&!output.payload);
    xr_xir_value_drop(&executable);xr_xir_value_drop(&arguments);uint64_t started=xr_time_monotonic_ns();CHECK(started);
    for(;;) {
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        if(closed!=XR_XIR_CALL_CLOSE_FAILED) { CHECK(closed==XR_XIR_CALL_OOM);break; }
        uint64_t now=xr_time_monotonic_ns();CHECK(now>=started&&now-started<UINT64_C(10000000000));xr_thread_yield();
    }
    CHECK(process_oom_prepares==1&&process_oom_starts==1);process_oom_observing=false;process_oom_child=NULL;
    process_child_done(child.leader,0,false);process_child_close(&child);CHECK(runtime_live==live&&runtime_bytes==bytes);
}
static void process_execute(XrXirProgram *program,const uint32_t *ids) {
    size_t live=runtime_live,bytes=runtime_bytes;
    process_trace("basic enter");process_basic(program,ids,PROCESS_SPAWN,"hold");process_basic(program,ids,PROCESS_SPAWN,"zero");
    process_basic(program,ids,PROCESS_CONFIGURED,"configured");process_trace("basic exit");
    process_trace("default_args enter");process_wait_resume(program,ids,"noargs");process_trace("default_args exit");
    process_trace("wait_resume enter");process_wait_resume(program,ids,"hold");process_trace("wait_resume exit");
    process_trace("streams enter");process_streams(program,ids);process_trace("streams exit");
    process_trace("kill enter");process_kill_and_retire(program,ids,true);process_kill_and_retire(program,ids,false);process_trace("kill exit");
    process_trace("tree enter");process_detached_and_tree(program,ids,false);process_trace("tree exit");
    process_trace("detached enter");process_detached_and_tree(program,ids,true);process_trace("detached exit");
    process_trace("refusals enter");process_refusals(program,ids);process_trace("refusals exit");
    process_trace("cancel enter");process_cancel(program,ids);process_trace("cancel exit");
    process_trace("privateHost enter");process_private_host(program,ids);process_trace("privateHost exit");
    process_trace("post_start_oom enter");process_result_oom(program,ids);process_trace("post_start_oom exit");
    CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#else
static bool process_child(int argc,char **argv) {(void)argc;(void)argv;return false;}
static void process_execute(XrXirProgram *program,const uint32_t *ids) {
    (void)program;(void)ids;fprintf(stderr,"Process child cooperation is not implemented on this platform\n");exit(2);
}
#endif

#endif // PROCESS_RUNTIME_H
