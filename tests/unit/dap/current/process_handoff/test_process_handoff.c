/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_process_handoff.c - Real worker survival after its frontend dies
 */
#include "os/os_proc.h"
#include "base/xmalloc.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s win=%lu\n",__LINE__,#c,GetLastError());exit(1); } } while (0)

#ifdef XDAP_PROCESS_INJECTED
static unsigned handoff_os_fail,release_queries,release_updates;
static DWORD creation_flags;
static XrProcId *reentry_owner;
static void handoff_reenter(void) {
    if (!reentry_owner) return;
    XrProcId *owner=reentry_owner;reentry_owner=NULL;XrProcId held=*owner;int code=-99;
    CHECK(xr_proc_release(owner)==XR_PROC_INVALID_ARGUMENT && *owner==held);
    CHECK(xr_proc_close(held)==-1 && xr_proc_kill_tree(held,9)==-1);
    CHECK(xr_proc_try_wait(held,&code)==XR_PROC_WAIT_ERROR && code==-99);
}
static BOOL handoff_query(HANDLE job,JOBOBJECTINFOCLASS kind,LPVOID data,DWORD size,LPDWORD written) {
    ++release_queries;handoff_reenter();
    if (handoff_os_fail==1) { handoff_os_fail=0;SetLastError(ERROR_READ_FAULT);return FALSE; }
    return QueryInformationJobObject(job,kind,data,size,written);
}
static BOOL handoff_update(HANDLE job,JOBOBJECTINFOCLASS kind,LPVOID data,DWORD size) {
    ++release_updates;
    if (handoff_os_fail==2) { handoff_os_fail=0;SetLastError(ERROR_READ_FAULT);return FALSE; }
    return SetInformationJobObject(job,kind,data,size);
}
/* Model an error reported after an actual cleanup side effect. Both native
 * handles really close, so the transaction status cannot hide a leaked handle. */
static BOOL handoff_close(HANDLE handle) {
    BOOL closed=CloseHandle(handle);
    if (closed && handoff_os_fail>=3) {
        if (handoff_os_fail==3) { handoff_os_fail=0;SetLastError(ERROR_READ_FAULT);return FALSE; }
        --handoff_os_fail;
    }
    return closed;
}
static BOOL handoff_create(LPCWSTR app,LPWSTR command,LPSECURITY_ATTRIBUTES process_security,
    LPSECURITY_ATTRIBUTES thread_security,BOOL inherit,DWORD flags,LPVOID environment,LPCWSTR cwd,
    LPSTARTUPINFOW startup,LPPROCESS_INFORMATION process) {
    creation_flags=flags;
    return CreateProcessW(app,command,process_security,thread_security,inherit,flags,environment,cwd,startup,process);
}
#define QueryInformationJobObject handoff_query
#define SetInformationJobObject handoff_update
#define CreateProcessW handoff_create
#define CloseHandle handoff_close
#include "os/win/proc_win.c"
#undef CloseHandle
#undef CreateProcessW
#undef SetInformationJobObject
#undef QueryInformationJobObject
#endif

typedef struct HandoffCase {
    char names[3][160];
    HANDLE events[3];
    XrPipe output,error;
} HandoffCase;
static unsigned case_number;
static char executable[32768];
static const char output_bytes[]="o\0\xe4\xb8\xad\n";
static const char error_bytes[]="e\0\xf0\x9f\x8c\x8d\n";
static void handoff_events(HandoffCase *c) {
    memset(c,0,sizeof(*c));c->output=c->error=(XrPipe){XR_PIPE_INVALID,XR_PIPE_INVALID};
    for (unsigned i=0;i<3;++i) {
        int n=snprintf(c->names[i],sizeof(c->names[i]),"Local\\XrayP16Handoff-%lu-%llu-%u-%u",
            GetCurrentProcessId(),(unsigned long long)GetTickCount64(),case_number,i);
        CHECK(n>0 && (size_t)n<sizeof(c->names[i]));
        c->events[i]=CreateEventA(NULL,TRUE,FALSE,c->names[i]);CHECK(c->events[i] && GetLastError()!=ERROR_ALREADY_EXISTS);
    }
    ++case_number;
    CHECK(!xr_pipe_create(&c->output,NULL) && !xr_pipe_create(&c->error,NULL));
}
static void handoff_cleanup(HandoffCase *c) {
    for (unsigned i=0;i<3;++i) CHECK(CloseHandle(c->events[i]));
    CHECK(!xr_pipe_close(c->output.read) && !xr_pipe_close(c->output.write));
    CHECK(!xr_pipe_close(c->error.read) && !xr_pipe_close(c->error.write));
}
static void handoff_write(HANDLE stream,const char *bytes,size_t size) {
    while (size) { DWORD n=0;CHECK(WriteFile(stream,bytes,(DWORD)size,&n,NULL) && n);bytes+=n;size-=n; }
}
static void handoff_line(const HandoffCase *c,const char *stage,char text[64]) {
    size_t n=0;
    while (n<63) {
        if (xr_pipe_read(c->output.read,&text[n],1)!=1) {
            char error[1024];int64_t size=xr_pipe_read(c->error.read,error,sizeof(error));
            fprintf(stderr,"frontend %s failed; partial=%.*s stderr=%.*s\n",stage,(int)n,text,size>0?(int)size:0,error);
            CHECK(false);
        }
        if (text[n++]=='\n') { text[n]=0;return; }
    }
    CHECK(false);
}
static void handoff_output(XrPipeHandle pipe,const char *expected,size_t length) {
    char bytes[64];size_t used=0;
    for (;;) { CHECK(used<sizeof(bytes));int64_t n=xr_pipe_read(pipe,bytes+used,sizeof(bytes)-used);CHECK(n>=0);if (!n) break;used+=(size_t)n; }
    CHECK(used==length && !memcmp(bytes,expected,length));
}
static XrProcId handoff_spawn(const HandoffCase *c,const char *mode,const char *extra,bool group) {
    const char *argv[]={executable,mode,c->names[0],c->names[1],c->names[2],extra,NULL};
    XrProcSpawnOptions options={.memory=xr_proc_system_memory(),.has_stdout=true,.stdout_write=c->output.write,
        .has_stderr=true,.stderr_write=c->error.write,.new_process_group=group,.no_console_window=true};
    XrProcId pid=XR_PROC_INVALID;CHECK(xr_proc_spawn(executable,argv,&options,&pid)==XR_PROC_OK && pid>0);
#ifdef XDAP_PROCESS_INJECTED
    CHECK((creation_flags&CREATE_NO_WINDOW)!=0);
    CHECK(((creation_flags&CREATE_NEW_PROCESS_GROUP)!=0)==group);
#endif
    return pid;
}
static int handoff_worker(char **argv) {
    HANDLE ready=OpenEventA(EVENT_MODIFY_STATE,FALSE,argv[2]),go=OpenEventA(SYNCHRONIZE,FALSE,argv[3]);
    CHECK(ready && go && !GetConsoleWindow());
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    CHECK(QueryInformationJobObject(NULL,JobObjectExtendedLimitInformation,&limits,sizeof(limits),NULL) &&
        (limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE));
    CHECK(SetEvent(ready) && WaitForSingleObject(go,30000)==WAIT_OBJECT_0);
    CHECK(QueryInformationJobObject(NULL,JobObjectExtendedLimitInformation,&limits,sizeof(limits),NULL) &&
        !(limits.BasicLimitInformation.LimitFlags&JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE));
    handoff_write(GetStdHandle(STD_OUTPUT_HANDLE),output_bytes,(sizeof(output_bytes)-1));
    handoff_write(GetStdHandle(STD_ERROR_HANDLE),error_bytes,(sizeof(error_bytes)-1));
    CHECK(CloseHandle(ready) && CloseHandle(go));return 23;
}
static int handoff_frontend(char **argv) {
    CHECK(!GetConsoleWindow());
    HandoffCase c={0};
    for (unsigned i=0;i<3;++i) { CHECK(strlen(argv[i+2])<sizeof(c.names[i]));strcpy(c.names[i],argv[i+2]); }
    c.output.write=(XrPipeHandle)(intptr_t)GetStdHandle(STD_OUTPUT_HANDLE);
    c.error.write=(XrPipeHandle)(intptr_t)GetStdHandle(STD_ERROR_HANDLE);
    HANDLE ready=OpenEventA(SYNCHRONIZE,FALSE,c.names[0]),transfer=OpenEventA(SYNCHRONIZE,FALSE,c.names[2]);CHECK(ready && transfer);
    XrProcId worker=handoff_spawn(&c,"--worker","",true);CHECK(WaitForSingleObject(ready,30000)==WAIT_OBJECT_0);
    char line[64];int n=snprintf(line,sizeof(line),"%llu\n",(unsigned long long)worker);CHECK(n>0 && (size_t)n<sizeof(line));
    handoff_write(GetStdHandle(STD_OUTPUT_HANDLE),line,(size_t)n);
    CHECK(WaitForSingleObject(transfer,30000)==WAIT_OBJECT_0);
    XrProcId old=worker;
    if (!strcmp(argv[5],"close")) CHECK(!xr_proc_close(worker));
    else {
        CHECK(xr_proc_release(&worker)==XR_PROC_OK && worker==XR_PROC_INVALID);
        CHECK(xr_proc_close(old)==-1 && xr_proc_kill_tree(old,9)==-1 && xr_proc_try_wait(old,NULL)==XR_PROC_WAIT_ERROR);
        CHECK(xr_proc_release(&worker)==XR_PROC_INVALID_ARGUMENT);
    }
    CHECK(CloseHandle(ready) && CloseHandle(transfer));
    handoff_write(GetStdHandle(STD_OUTPUT_HANDLE),"transferred\n",12);
    if (!strcmp(argv[5],"kill")) Sleep(INFINITE);
    return 7;
}
static void handoff_frontend_death(const char *mode) {
    HandoffCase c;handoff_events(&c);XrProcId frontend=handoff_spawn(&c,"--frontend",mode,false);
    CHECK(!xr_pipe_close(c.output.write) && !xr_pipe_close(c.error.write));c.output.write=c.error.write=XR_PIPE_INVALID;
    char line[64];handoff_line(&c,"PID",line);unsigned long long id=0;CHECK(sscanf(line,"%llu",&id)==1 && id>0 && id<=UINT32_MAX);
    HANDLE worker=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)id);CHECK(worker);
    CHECK(SetEvent(c.events[2]));handoff_line(&c,"transfer",line);CHECK(!strcmp(line,"transferred\n"));
    if (!strcmp(mode,"kill")) CHECK(!xr_proc_kill(frontend,9));
    int code=-99;CHECK(!xr_proc_wait(frontend,&code) && code==(!strcmp(mode,"kill")?-1:7));
    if (!strcmp(mode,"close")) {
        CHECK(WaitForSingleObject(worker,30000)==WAIT_OBJECT_0);
        handoff_output(c.output.read,"",0);handoff_output(c.error.read,"",0);
    } else {
        CHECK(WaitForSingleObject(worker,0)==WAIT_TIMEOUT && SetEvent(c.events[1]));
        handoff_output(c.output.read,output_bytes,(sizeof(output_bytes)-1));handoff_output(c.error.read,error_bytes,(sizeof(error_bytes)-1));
        CHECK(WaitForSingleObject(worker,30000)==WAIT_OBJECT_0);DWORD result=0;CHECK(GetExitCodeProcess(worker,&result) && result==23);
    }
    CHECK(CloseHandle(worker));handoff_cleanup(&c);
    printf("real frontend=%s worker=%s output UTF8/NUL channels exact PASS\n",mode,!strcmp(mode,"close")?"terminated":"completed23");
}

static void handoff_local(unsigned failure,bool reenter) {
    HandoffCase c;handoff_events(&c);XrProcId pid=handoff_spawn(&c,"--worker","",true),held=pid;
    CHECK(WaitForSingleObject(c.events[0],30000)==WAIT_OBJECT_0);
    HANDLE worker=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)pid);CHECK(worker);
#ifdef XDAP_PROCESS_INJECTED
    handoff_os_fail=failure;reentry_owner=reenter ? &pid : NULL;
#else
    CHECK(!failure);(void)reenter;
#endif
    XrOsProcStatus status=xr_proc_release(&pid);
#ifdef XDAP_PROCESS_INJECTED
    CHECK(!reentry_owner);
#endif
    if (status!=XR_PROC_OK) {
        CHECK(status==XR_PROC_IO && WaitForSingleObject(worker,0)==WAIT_TIMEOUT);
        if (failure>=3) {
            CHECK(pid==XR_PROC_INVALID);
            CHECK(xr_proc_close(held)==-1 && xr_proc_try_wait(held,NULL)==XR_PROC_WAIT_ERROR);
        } else {
            CHECK(pid==held);int sentinel=-99;
            CHECK(xr_proc_try_wait(pid,&sentinel)==XR_PROC_WAIT_RUNNING && sentinel==-99);
            CHECK(xr_proc_release(&pid)==XR_PROC_OK);
        }
    }
    CHECK(pid==XR_PROC_INVALID);
    CHECK(SetEvent(c.events[1]) && WaitForSingleObject(worker,30000)==WAIT_OBJECT_0);
    DWORD result=0;CHECK(GetExitCodeProcess(worker,&result) && result==23);
    CHECK(!xr_pipe_close(c.output.write) && !xr_pipe_close(c.error.write));c.output.write=c.error.write=XR_PIPE_INVALID;
    handoff_output(c.output.read,output_bytes,sizeof(output_bytes)-1);
    handoff_output(c.error.read,error_bytes,sizeof(error_bytes)-1);
    CHECK(CloseHandle(worker));handoff_cleanup(&c);
}
static XrOsProcStatus handoff_image(void *context,const XrProcImageEvent *event) {
    (void)context;CHECK(event && event->file_handle);return XR_PROC_OK;
}
static void handoff_platform_warmup(void) {
    /* Initialize the existing debug and transport paths before measuring owned
     * handles. These calls use spawn/close and never execute release. */
    const char *argv[]={executable,"--passive",NULL};
    XrProcSpawnOptions options={.memory=xr_proc_system_memory(),.new_process_group=true,
        .image_mode=XR_PROC_IMAGES_WINDOWS_TREE,.image_observer={NULL,handoff_image},.no_console_window=true};
    XrProcId pid=XR_PROC_INVALID;CHECK(xr_proc_spawn(executable,argv,&options,&pid)==XR_PROC_OK && !xr_proc_close(pid));
    handoff_frontend_death("close");
}
static void handoff_owner_controls(void) {
    const char *argv[]={executable,"--passive",NULL};
    XrProcSpawnOptions options={.memory=xr_proc_system_memory(),.no_console_window=true};
    XrProcId pid=XR_PROC_INVALID;CHECK(xr_proc_spawn(executable,argv,&options,&pid)==XR_PROC_OK);
    XrProcId held=pid;
    CHECK(xr_proc_release(&pid)==XR_PROC_INVALID_ARGUMENT && pid==held);
    int result=-99;CHECK(!xr_proc_wait(pid,&result) && result==19);
    CHECK(xr_proc_release(&pid)==XR_PROC_INVALID_ARGUMENT && pid==held);
    options.new_process_group=true;pid=XR_PROC_INVALID;
    CHECK(xr_proc_spawn(executable,argv,&options,&pid)==XR_PROC_OK && !xr_proc_wait(pid,&result) && result==19);
    CHECK(xr_proc_try_wait(pid,NULL)==XR_PROC_WAIT_ERROR && xr_proc_release(&pid)==XR_PROC_OK && pid==XR_PROC_INVALID);
    options.image_mode=XR_PROC_IMAGES_WINDOWS_TREE;options.image_observer=(XrProcImageObserver){NULL,handoff_image};
    CHECK(xr_proc_spawn(executable,argv,&options,&pid)==XR_PROC_OK);held=pid;
    CHECK(xr_proc_release(&pid)==XR_PROC_UNSUPPORTED && pid==held);
    CHECK(!xr_proc_close(pid));
    puts("ordinary/consumed owner refusal, waited group release and observed-group unsupported preserve owner PASS");
}
int main(int argc,char **argv) {
    CHECK(!xr_proc_self_exe_path(executable,sizeof(executable)));
    if (argc==2 && !strcmp(argv[1],"--passive")) return 19;
    if (argc==6 && !strcmp(argv[1],"--worker")) return handoff_worker(argv);
    if (argc==6 && !strcmp(argv[1],"--frontend")) return handoff_frontend(argv);
    bool resources=argc==2 && !strcmp(argv[1],"--resources");CHECK(argc==1 || resources);
    DWORD handles=0,after=0,cold=0,samples[5]={0};CHECK(GetProcessHandleCount(GetCurrentProcess(),&cold));
    handoff_platform_warmup();CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles));
    XrProcId invalid=XR_PROC_INVALID;
    CHECK(xr_proc_release(NULL)==XR_PROC_INVALID_ARGUMENT);
    CHECK(xr_proc_release(&invalid)==XR_PROC_INVALID_ARGUMENT && invalid==XR_PROC_INVALID);
    invalid=(XrProcId)GetCurrentProcessId();CHECK(xr_proc_release(&invalid)==XR_PROC_INVALID_ARGUMENT && invalid==(XrProcId)GetCurrentProcessId());
    handoff_owner_controls();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&samples[0]));
    handoff_frontend_death("close");handoff_frontend_death("exit");handoff_frontend_death("kill");
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&samples[1]));
    handoff_local(0,true);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&samples[2]));
    handoff_local(0,false);
#ifdef XDAP_PROCESS_INJECTED
    handoff_local(1,true);
    if (resources) for (unsigned failure=2;failure<=4;++failure) handoff_local(failure,true);
    for (unsigned i=0;i<XR_PROC_MAX_LIVE;++i) CHECK(!g_live[i].pid);
#endif
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&samples[3]));
    fprintf(stderr,"handle samples cold=%lu baseline=%lu controls=%lu frontend=%lu local=%lu final=%lu; warmup uses no release\n",
        cold,handles,samples[0],samples[1],samples[2],samples[3]);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && handles==after);
    printf("owned job release: no allocation, reentry refusal, OS IO retry, frontend PID death and handle0; OS injection=%s PASS\n",
        resources?"ALL_RELEASE_OS_BOUNDARIES":"QUERY_ONLY");return 0;
}
