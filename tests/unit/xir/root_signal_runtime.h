/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * root_signal_runtime.h - Observable owned registration and deferred handler cases
 */
#ifndef ROOT_SIGNAL_RUNTIME_H
#define ROOT_SIGNAL_RUNTIME_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_instance_signal_internal.h"
#include "xir/xxir_program_packet_internal.h"
#include "xir/xxir_root_callback_internal.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include "execution/xr_xir_host_time.h"
#include "os/os_time.h"
#include "os/os_signal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#include <wchar.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Root signal %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static XrOsIoStatus root_signal_close(XrOsSignalOwner *owner);
#define xr_os_io_signal_close root_signal_close
#include "xir_runtime_allocations.h"
#undef xr_os_io_signal_close

static XrXirInstance *signal_closing_instance;
static unsigned signal_close_failures,signal_close_failures_seen;
static XrOsIoStatus root_signal_close(XrOsSignalOwner *owner) {
    if (owner->token && signal_closing_instance)
        CHECK(xr_xir_instance_free(signal_closing_instance)==XR_XIR_CALL_BUSY);
    if (owner->token && signal_close_failures) {
        --signal_close_failures;++signal_close_failures_seen;return XR_OS_IO_IO;
    }
    return xr_os_io_signal_close(owner);
}
static const char root_signal_text[]=
 "import time\nvar state:i64=0\n"
 "fn worker(value:i64)->i64{Coro.yield();time.sleep(2);return value}\n"
 "fn make(mode:i64)->fn(){var captured:i64=41;return fn(){"
 "defer{print(\"callback-cleanup\");if(mode==3){assert(false)}};print(\"callback-enter\");"
 "captured+=1;let task=go worker(captured);let value=await task;"
 "if(mode==1||mode==3||mode==5){assert(false)};if(mode==2){time.sleep(100)};"
 "if(mode==4){let next=fn(){defer{print(\"callback-cleanup\")};print(\"callback-enter\");"
 "captured+=1;state=captured;print(\"callback-returned\")};assert(__onSignal(2,next));print(\"replaced\");"
 "print(\"callback-returned\");return};state=value;print(\"callback-returned\")}}\n"
 "export fn parent(mode:i64)->i64{defer{print(\"parent-cleanup\");if(mode==5){assert(false)}};"
 "let handler=make(mode);let register=withHandler;if(!register(handler)){print(\"refused\");return -2};"
 "print(\"registered\");while(state==0){time.sleep(1)};return state}\n"
 "export fn guarded(mode:i64)->i64{try{return parent(mode)}catch panic{print(\"unexpected-catch\");return -1}}\n"
 "export fn owned()->fn(){return make(0)}\n"
 "export fn withHandler(handler:fn())->bool{return __onSignal(2,handler)}\n"
 "export fn read()->i64{return state}\n";
typedef struct RootSignalLog {
    unsigned registered,entered,returned,callback_cleanup,parent_cleanup,replaced,refused,unexpected;
} RootSignalLog;
static XrXirOutputStatus root_signal_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    RootSignalLog *log=context;
    if (stream!=XR_XIR_STDOUT) {++log->unexpected;return XR_XIR_OUTPUT_OK;}
    const char *names[]={"registered\n","callback-enter\n","callback-returned\n","callback-cleanup\n",
        "parent-cleanup\n","replaced\n","refused\n"};
    unsigned *counts[]={&log->registered,&log->entered,&log->returned,&log->callback_cleanup,
        &log->parent_cleanup,&log->replaced,&log->refused};
    for (unsigned i=0;i<7;++i) if (strlen(names[i])==length && !memcmp(names[i],bytes,length)) {
        ++*counts[i];return XR_XIR_OUTPUT_OK;
    }
    ++log->unexpected;return XR_XIR_OUTPUT_OK;
}
static XrXirInstance *root_signal_instance(XrXirProgram *program,XrXirOutputSink *sink) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static void root_signal_send(void) {
#if defined(XR_OS_WINDOWS)
    DWORD processes[2]={0};
    CHECK(GetConsoleProcessList(processes,2)==1 && processes[0]==GetCurrentProcessId());
    CHECK(GenerateConsoleCtrlEvent(CTRL_C_EVENT,0));
#else
    CHECK(raise(SIGINT)==0);
#endif
}
static XrXirInstanceResult root_signal_step(XrXirInstance *instance) {
    XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,1);
    if (result.outcome.status==XR_XIR_CALL_SUSPENDED) {
        XrXirWaitRequest request={0};
        CHECK(xr_xir_instance_wait_request(instance,result.epoch,result.outcome.wake,&request)==XR_XIR_CALL_READY);
        if (request.kind==XR_XIR_WAIT_TIMER_MS || request.kind==XR_XIR_WAIT_YIELD) {
            XirHostWaitDeadline deadline={0};
            CHECK(xir_instance_host_wait_deadline(instance,result.epoch,result.outcome.wake,
                request.kind==XR_XIR_WAIT_TIMER_MS?xr_time_monotonic_ns():0,&deadline)==XR_XIR_CALL_READY);
            XrXirHostWait wait={deadline.start_ns,deadline.duration_ns,deadline.active};
            XrXirHostWaitStatus due=xr_xir_host_wait_poll(&wait,UINT64_C(1000000));
            CHECK(due==XR_XIR_HOST_WAIT_DUE || due==XR_XIR_HOST_WAIT_PENDING);
            if (due==XR_XIR_HOST_WAIT_DUE)
                CHECK(xr_xir_instance_resume(instance,result.epoch,result.outcome.wake)==XR_XIR_CALL_READY);
        } else CHECK(request.kind==XR_XIR_WAIT_CHANNEL);
    }
    return result;
}
static XrXirInstanceResult root_signal_until(XrXirInstance *instance,RootSignalLog *log,bool registered) {
    uint64_t deadline=xr_time_monotonic_ns()+UINT64_C(10000000000);
    for (;;) {
        CHECK(xr_time_monotonic_ns()<deadline);
        XrXirInstanceResult result=root_signal_step(instance);
        if (registered && log->registered) return result;
        if (result.outcome.status!=XR_XIR_CALL_READY && result.outcome.status!=XR_XIR_CALL_SUSPENDED) return result;
    }
}
static void root_signal_cases(XrXirProgram *program,const uint32_t ids[6]) {
    bool untouched=true;
    CHECK(xir_instance_signal_register(NULL,&untouched)==XR_XIR_CALL_BAD_ARGUMENT && untouched);
    for (unsigned mode=0;mode<6;++mode) {
        size_t baseline=runtime_live,bytes=runtime_bytes;RootSignalLog log={0};
        XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,root_signal_output,&log,4096};
        XrXirInstance *instance=root_signal_instance(program,&sink);
        XrXirValue argument={XR_XIR_I64,0,mode};
        CHECK(xr_xir_instance_start(instance,ids[mode==1||mode==5?1:0],&argument,1)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=root_signal_until(instance,&log,true);
        CHECK(log.registered==1 && !log.refused);
        root_signal_send();bool cancelled=false,repeated=false;
        uint64_t deadline=xr_time_monotonic_ns()+UINT64_C(10000000000);
        do {
            CHECK(xr_time_monotonic_ns()<deadline);result=root_signal_step(instance);
            if (mode==2 && log.entered && !cancelled &&
                (result.outcome.status==XR_XIR_CALL_READY || result.outcome.status==XR_XIR_CALL_SUSPENDED)) {
                CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);cancelled=true;
            }
            if (mode==4 && log.replaced && !repeated) {root_signal_send();repeated=true;}
        } while (result.outcome.status==XR_XIR_CALL_READY || result.outcome.status==XR_XIR_CALL_SUSPENDED);
        XrXirCallStatus expected=mode==0||mode==4?XR_XIR_CALL_RETURNED:mode==1?XR_XIR_CALL_ASSERTION:
            mode==2?XR_XIR_CALL_CANCELLED:XR_XIR_CALL_CLEANUP_FAILED;
        CHECK(result.outcome.status==expected && !log.unexpected && xr_xir_call_result_valid(&result.outcome));
        if (mode==3 || mode==5) CHECK(result.outcome.prior_status==XR_XIR_CALL_ASSERTION &&
            xr_xir_call_panic_payload(result.outcome.prior_status,&result.outcome.prior_panic));
        if (expected==XR_XIR_CALL_RETURNED) {
            XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED &&
                value.type==XR_XIR_I64 && value.payload==(mode==4?43:42));xr_xir_value_drop(&value);
        }
        CHECK(log.entered==(mode==4?2u:1u) && log.callback_cleanup==(mode==4?2u:1u) &&
            log.returned==(mode==4?2u:mode==0?1u:0u) && log.parent_cleanup==(mode==3?0u:1u));
        XrXirCallStatus freed=xr_xir_instance_free(instance);
        CHECK(freed==XR_XIR_CALL_READY || freed==expected);
        CHECK(runtime_live==baseline && runtime_bytes==bytes);
    }
}
static void root_signal_stop_cases(XrXirProgram *program,const uint32_t ids[6]) {
    for (unsigned free_direct=0;free_direct<2;++free_direct) {
        size_t baseline=runtime_live,bytes=runtime_bytes;RootSignalLog log={0};
        XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,root_signal_output,&log,4096};
        XrXirInstance *instance=root_signal_instance(program,&sink);XrXirValue mode={XR_XIR_I64,0,2};
        CHECK(xr_xir_instance_start(instance,ids[0],&mode,1)==XR_XIR_CALL_READY);
        (void)root_signal_until(instance,&log,true);CHECK(log.registered==1);root_signal_send();
        uint64_t deadline=xr_time_monotonic_ns()+UINT64_C(10000000000);
        while (!log.entered) {
            CHECK(xr_time_monotonic_ns()<deadline);XrXirCallStatus status=root_signal_step(instance).outcome.status;
            CHECK(status==XR_XIR_CALL_READY || status==XR_XIR_CALL_SUSPENDED);
        }
        if (!free_direct) {
            CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_CANCELLED);
        }
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        CHECK(closed==XR_XIR_CALL_READY || closed==XR_XIR_CALL_CANCELLED);
        CHECK(log.entered==1 && !log.returned && log.callback_cleanup==1 && log.parent_cleanup==1 && !log.unexpected);
        CHECK(runtime_live==baseline && runtime_bytes==bytes);
    }
}
static void root_signal_owner_cases(XrXirProgram *program,const uint32_t ids[6],const XrXirCompileContext *context) {
    for (unsigned failures=1;failures<=2;++failures) {
        size_t baseline=runtime_live,bytes=runtime_bytes;RootSignalLog log={0};
        XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,root_signal_output,&log,4096};
        XrXirInstance *instance=root_signal_instance(program,&sink);signal_closing_instance=instance;
        signal_close_failures=failures;signal_close_failures_seen=0;
        XrXirValue mode={XR_XIR_I64,0,0};CHECK(xr_xir_instance_start(instance,ids[0],&mode,1)==XR_XIR_CALL_READY);
        (void)root_signal_until(instance,&log,true);CHECK(log.registered==1);root_signal_send();
        CHECK(root_signal_until(instance,&log,false).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirCallStatus closed=xr_xir_instance_free(instance);
        if (failures==2) {CHECK(closed==XR_XIR_CALL_CLOSE_FAILED);closed=xr_xir_instance_free(instance);}
        signal_closing_instance=NULL;CHECK(closed==XR_XIR_CALL_READY && signal_close_failures_seen==failures);
        CHECK(log.entered==1 && log.returned==1 && log.callback_cleanup==1 && log.parent_cleanup==1 && !log.unexpected);
        CHECK(runtime_live==baseline && runtime_bytes==bytes);
    }
    RootSignalLog first_log={0},other_log={0};
    XrXirOutputSink first_sink={XR_XIR_CALL_ABI_VERSION,0,root_signal_output,&first_log,4096};
    XrXirOutputSink other_sink={XR_XIR_CALL_ABI_VERSION,0,root_signal_output,&other_log,4096};
    XrXirInstance *first=root_signal_instance(program,&first_sink),*other=root_signal_instance(program,&other_sink);
    XrXirValue mode={XR_XIR_I64,0,0};CHECK(xr_xir_instance_start(first,ids[0],&mode,1)==XR_XIR_CALL_READY);
    (void)root_signal_until(first,&first_log,true);CHECK(first_log.registered==1);
    CHECK(xr_xir_instance_start(other,ids[0],&mode,1)==XR_XIR_CALL_READY);
    CHECK(root_signal_until(other,&other_log,false).outcome.status==XR_XIR_CALL_RETURNED &&
        other_log.refused==1 && !other_log.registered && !other_log.entered);
    XrXirValue refused={0};CHECK(xr_xir_instance_take_result(other,&refused)==XR_XIR_CALL_RETURNED &&
        refused.type==XR_XIR_I64 && refused.payload==-2);xr_xir_value_drop(&refused);
    CHECK(xr_xir_instance_free(other)==XR_XIR_CALL_READY);
    const XrXirArtifact *actual=program->permissions->input->artifact;
    XrXirProgramSpec descriptor={XR_XIR_PROGRAM_ABI_VERSION,actual->target,program->entries,
        program->entry_count,program->declarations,program->code,program->types,xir_program_packet_borrow(actual)};
    XrXirProgram *received=NULL;CHECK(xr_xir_compile_program_seal(context,&descriptor,&received)==XR_XIR_OK);
    uint64_t generation=0;CHECK(xr_xir_instance_generation_prepare(first,received,&generation)==XR_XIR_CALL_BAD_STATE && !generation);
    root_signal_send();CHECK(root_signal_until(first,&first_log,false).outcome.status==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_prepare(first,received,&generation)==XR_XIR_CALL_READY && generation);
    CHECK(xr_xir_instance_start(first,ids[5],NULL,0)==XR_XIR_CALL_READY);
    CHECK(root_signal_until(first,&first_log,false).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(first,&value)==XR_XIR_CALL_RETURNED &&
        value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_generation_publish(first,generation)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(received);
    CHECK(!runtime_live && !runtime_bytes);
}
static XrXirValue root_signal_owned(XrXirInstance *instance,uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    RootSignalLog unused={0};CHECK(root_signal_until(instance,&unused,false).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue function={0};CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_function_binding(&function));return function;
}
static void root_signal_admission_cases(XrXirProgram *program,const uint32_t ids[6]) {
    RootSignalLog log={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,root_signal_output,&log,4096};
    XrXirInstance *first=root_signal_instance(program,&sink),*other=root_signal_instance(program,&sink);
    XrXirValue owned=root_signal_owned(first,ids[2]),foreign=root_signal_owned(other,ids[2]);
    XrXirValue arguments[]={{XR_XIR_I64,0,2},owned};
    CHECK(xr_xir_instance_start(first,ids[4],arguments,2)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(first,ids[3],&foreign,1)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(first,ids[3],&owned,1)==XR_XIR_CALL_READY);
    CHECK(root_signal_until(first,&log,false).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue installed={0};CHECK(xr_xir_instance_take_result(first,&installed)==XR_XIR_CALL_RETURNED &&
        installed.type==XR_XIR_BOOL && installed.payload==1);xr_xir_value_drop(&installed);
    xr_xir_value_drop(&owned);xr_xir_value_drop(&foreign);
    CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY && xr_xir_instance_free(other)==XR_XIR_CALL_READY);
    CHECK(!log.entered && !log.callback_cleanup && !log.unexpected && !runtime_live && !runtime_bytes);
}
static void root_signal_detached(XrXirProgram *program,const uint32_t ids[6]) {
    RootSignalLog log={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,root_signal_output,&log,4096};
    XrXirInstance *instance=root_signal_instance(program,&sink);XrXirValue mode={XR_XIR_I64,0,0};
    CHECK(xr_xir_instance_start(instance,ids[0],&mode,1)==XR_XIR_CALL_READY);
    CHECK(root_signal_until(instance,&log,false).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED &&
        value.type==XR_XIR_I64 && value.payload==-2);xr_xir_value_drop(&value);
    CHECK(log.refused==1 && log.parent_cleanup==1 && !log.registered && !log.entered && !log.unexpected);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && !runtime_live && !runtime_bytes);
}
#if defined(XR_OS_WINDOWS)
static BOOL WINAPI root_signal_previous(DWORD event) { return event==CTRL_C_EVENT?TRUE:FALSE; }
static void root_signal_isolate(const wchar_t *mode,DWORD flags) {
    wchar_t executable[32768],command[32768];DWORD length=GetModuleFileNameW(NULL,executable,32768);
    CHECK(length && length<32768 && swprintf(command,32768,L"\"%ls\" %ls",executable,mode)>0);
    HANDLE job=CreateJobObjectW(NULL,NULL);CHECK(job);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    CHECK(SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)));
    STARTUPINFOW startup={0};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION process={0};
    CHECK(CreateProcessW(executable,command,NULL,NULL,FALSE,flags|CREATE_SUSPENDED,NULL,NULL,&startup,&process));
    if (!AssignProcessToJobObject(job,process.hProcess)) {
        (void)TerminateProcess(process.hProcess,99);(void)WaitForSingleObject(process.hProcess,10000);CHECK(false);
    }
    CHECK(ResumeThread(process.hThread)!=UINT32_MAX && CloseHandle(process.hThread));
    DWORD waited=WaitForSingleObject(process.hProcess,20000),code=UINT32_MAX;
    if (waited!=WAIT_OBJECT_0) {CHECK(TerminateJobObject(job,99));CHECK(WaitForSingleObject(process.hProcess,10000)==WAIT_OBJECT_0);}
    CHECK(GetExitCodeProcess(process.hProcess,&code));CHECK(CloseHandle(process.hProcess));
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting={0};
    CHECK(QueryInformationJobObject(job,JobObjectBasicAccountingInformation,&accounting,sizeof(accounting),NULL));
    CHECK(!accounting.ActiveProcesses && CloseHandle(job));CHECK(waited==WAIT_OBJECT_0 && !code);
}
#endif
#endif // ROOT_SIGNAL_RUNTIME_H
