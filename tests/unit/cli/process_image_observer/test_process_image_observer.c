/* Real debug events, borrowed image handles and failure cleanup. */
#include "base/xmalloc.h"
#include "app/toolchain/xtc_process.h"
#include "os/os_time.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s (win=%lu)\n",__LINE__,#c,GetLastError()); exit(1); } } while (0)
typedef struct Allocation { void *p; size_t size; } Allocation;
static Allocation allocations[4096];
static size_t calls, fail_at=SIZE_MAX, physical, allocation_count;
static void *observed_alloc(size_t size) {
    if (calls++==fail_at) return NULL;
    void *p=xr_malloc(size); CHECK(p);
    for (unsigned i=0;i<4096;++i) if (!allocations[i].p) {
        allocations[i]=(Allocation){p,size}; physical+=size; ++allocation_count; return p;
    }
    CHECK(false); return NULL;
}
static void observed_free(void *p) {
    if (!p) return;
    for (unsigned i=0;i<4096;++i) if (allocations[i].p==p) {
        physical-=allocations[i].size; --allocation_count; allocations[i]=(Allocation){0}; xr_free(p); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_alloc
#define xr_free observed_free
#include "base/xcompile_resources.c"

static size_t wait_calls, continue_calls, fail_wait_at=SIZE_MAX, fail_continue_at=SIZE_MAX;
static bool null_image, fail_assign, fail_resume, fail_accounting, fail_image_close;
static DWORD injected_error=ERROR_READ_FAULT;
static bool persistent_wait, persistent_child_exit;
static DWORD root_pid, blocked_exit_pid, detached_exit_pid;
static size_t event_count, first_chance_av, second_chance_av;
static bool profile_enabled;
static XrProcCompletionPolicy command_completion=XR_PROC_COMPLETE_TREE;
static size_t terminate_calls, root_exit_continues, child_exit_continues;
static bool fail_terminate, mismatch_accounting;
static DWORD pending_exit_pid;
static bool fail_root_continue;
static XrCompileResources *exhaust_at_root;
static uint64_t exhaust_work_limit;
static uint64_t sleep_ns, debug_wait_ns, pipe_ns, poll_ns, child_cpu_100ns;
static size_t sleeps, idle_polls;

static HANDLE image_handles[256];
static unsigned image_handle_count;
static void remember_image(HANDLE file) {
    if (!file || file==INVALID_HANDLE_VALUE) return;
    for (unsigned i=0;i<256;++i) if (!image_handles[i]) { image_handles[i]=file; ++image_handle_count; return; }
    CHECK(false);
}
static BOOL debug_wait(LPDEBUG_EVENT event,DWORD timeout) {
    if (persistent_wait || wait_calls++==fail_wait_at) { SetLastError(injected_error); return FALSE; }
    uint64_t started=profile_enabled ? xr_time_monotonic_ns() : 0;
    BOOL ok=WaitForDebugEvent(event,timeout);
    DWORD error=GetLastError();
    if (profile_enabled) {
        debug_wait_ns+=xr_time_monotonic_ns()-started;
        if (!ok && error==ERROR_SEM_TIMEOUT) ++idle_polls;
        if (ok && event->dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
            HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,event->dwProcessId);
            FILETIME creation,exit_time,kernel,user;
            if (process && GetProcessTimes(process,&creation,&exit_time,&kernel,&user))
                child_cpu_100ns+=((uint64_t)kernel.dwHighDateTime<<32)+kernel.dwLowDateTime+((uint64_t)user.dwHighDateTime<<32)+user.dwLowDateTime;
            if (process) CloseHandle(process);
        }
    }
    SetLastError(error);
    if (ok) {
        ++event_count;
        pending_exit_pid=event->dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT ? event->dwProcessId : 0;
        if (event->dwDebugEventCode==EXCEPTION_DEBUG_EVENT && event->u.Exception.ExceptionRecord.ExceptionCode==EXCEPTION_ACCESS_VIOLATION) {
            if (event->u.Exception.dwFirstChance) ++first_chance_av; else ++second_chance_av;
        }
        if (event->dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT && !root_pid) root_pid=event->dwProcessId;
        if (persistent_child_exit && event->dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT && event->dwProcessId!=root_pid)
            blocked_exit_pid=event->dwProcessId;
        HANDLE *file=event->dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT ? &event->u.CreateProcessInfo.hFile :
            event->dwDebugEventCode==LOAD_DLL_DEBUG_EVENT ? &event->u.LoadDll.hFile : NULL;
        if (file && *file) {
            if (null_image) { CHECK(CloseHandle(*file)); *file=NULL; null_image=false; }
            else remember_image(*file);
        }
    }
    return ok;
}
static BOOL debug_continue(DWORD pid,DWORD tid,DWORD status) {
    if ((persistent_child_exit && pid==blocked_exit_pid) || continue_calls++==fail_continue_at) { SetLastError(injected_error); return FALSE; }
    if (fail_root_continue && pid==pending_exit_pid && pid==root_pid) {
        fail_root_continue=false; SetLastError(injected_error); return FALSE;
    }
    BOOL okay=ContinueDebugEvent(pid,tid,status);
    if (okay && pid==pending_exit_pid) {
        if (pid==root_pid) ++root_exit_continues; else ++child_exit_continues;
        if (pid==root_pid && exhaust_at_root) {
            XrCompileResourceStats used;
            CHECK(xr_compile_resources_stats(exhaust_at_root,&used)==XR_COMPILE_RESOURCE_OK);
            CHECK(used.work<=exhaust_work_limit);
            CHECK(xr_compile_resources_work(exhaust_at_root,exhaust_work_limit-used.work)==XR_COMPILE_RESOURCE_OK);
            exhaust_at_root=NULL;
        }
        pending_exit_pid=0;
    }
    return okay;
}
static BOOL debug_close(HANDLE handle) {
    int found=-1;
    for (unsigned i=0;i<256;++i) if (image_handles[i]==handle) found=(int)i;
    if (found>=0 && fail_image_close) { fail_image_close=false; SetLastError(injected_error); return FALSE; }
    BOOL ok=CloseHandle(handle);
    if (ok && found>=0) { image_handles[found]=NULL; --image_handle_count; }
    return ok;
}
static BOOL debug_assign(HANDLE job,HANDLE process) {
    if (fail_assign) { fail_assign=false; SetLastError(injected_error); return FALSE; }
    return AssignProcessToJobObject(job,process);
}
static DWORD debug_resume(HANDLE thread) {
    if (fail_resume) { fail_resume=false; SetLastError(injected_error); return (DWORD)-1; }
    return ResumeThread(thread);
}
static BOOL debug_accounting(HANDLE job,JOBOBJECTINFOCLASS kind,LPVOID data,DWORD size,LPDWORD returned) {
    if (fail_accounting) { fail_accounting=false; SetLastError(injected_error); return FALSE; }
    BOOL okay=QueryInformationJobObject(job,kind,data,size,returned);
    if (okay && mismatch_accounting && kind==JobObjectBasicAccountingInformation)
        ++((JOBOBJECT_BASIC_ACCOUNTING_INFORMATION *)data)->TotalProcesses;
    return okay;
}
static BOOL debug_detach(DWORD pid) {
    BOOL result=DebugActiveProcessStop(pid);
    if (pid==blocked_exit_pid) detached_exit_pid=pid;
    return result;
}
static BOOL debug_terminate(HANDLE job,UINT code) {
    ++terminate_calls;
    if (fail_terminate) { fail_terminate=false; SetLastError(injected_error); return FALSE; }
    return TerminateJobObject(job,code);
}
#define TerminateJobObject debug_terminate
#define DebugActiveProcessStop debug_detach
#define WaitForDebugEvent debug_wait
#define ContinueDebugEvent debug_continue
#define CloseHandle debug_close
#define AssignProcessToJobObject debug_assign
#define ResumeThread debug_resume
#define QueryInformationJobObject debug_accounting
#include "os/win/proc_win.c"
#include "os/win/pipe_win.c"
static void measured_sleep(uint64_t milliseconds) {
    uint64_t start=profile_enabled ? xr_time_monotonic_ns() : 0;
    xr_time_sleep_ms(milliseconds);
    if (profile_enabled) { ++sleeps; sleep_ns+=xr_time_monotonic_ns()-start; }
}
static XrPipeIoStatus measured_probe(XrPipeHandle handle,size_t *available,bool *eof) {
    uint64_t start=profile_enabled ? xr_time_monotonic_ns() : 0;
    XrPipeIoStatus result=xr_pipe_probe(handle,available,eof); DWORD error=GetLastError();
    if (profile_enabled) pipe_ns+=xr_time_monotonic_ns()-start;
    SetLastError(error); return result;
}
static XrProcWaitResult measured_poll(XrProcId pid,int *code) {
    uint64_t start=profile_enabled ? xr_time_monotonic_ns() : 0;
    XrProcWaitResult result=xr_proc_try_wait(pid,code); DWORD error=GetLastError();
    if (profile_enabled) poll_ns+=xr_time_monotonic_ns()-start;
    SetLastError(error); return result;
}
#define xr_time_sleep_ms measured_sleep
#define xr_pipe_probe measured_probe
#define xr_proc_try_wait measured_poll
#include "app/toolchain/xtc_process.c"
#undef xr_time_sleep_ms
#undef xr_pipe_probe
#undef xr_proc_try_wait

#undef DebugActiveProcessStop
#undef WaitForDebugEvent
#undef ContinueDebugEvent
#undef CloseHandle
#undef AssignProcessToJobObject
#undef ResumeThread
#undef QueryInformationJobObject

static char executable[32768], directory[32768], dll_path[32768];
static const XrCompileResourceLimits unlimited={UINT64_MAX,UINT64_MAX,UINT64_MAX};
typedef struct ImageRecord { struct ImageRecord *next; HANDLE file; DWORD pid; bool dll; } ImageRecord;
typedef struct Observation {
    XrCompileResources *resources;
    ImageRecord *images;
    unsigned executables, dlls, fixture_loads;
    XrOsProcStatus reject;
    bool thread_checks, checked_threads, exhaust;
} Observation;

typedef struct ForeignCall { XrProcId pid; XrOsProcStatus status; XrProcImagePumpResult result; int closed; } ForeignCall;
static DWORD WINAPI foreign_call(void *context) {
    ForeignCall *c=context; c->status=xr_proc_pump_images(c->pid,&c->result); c->closed=xr_proc_close(c->pid); return 0;
}
static void reentry_check(Observation *o,const XrProcImageEvent *event) {
    XrProcImagePumpResult result={true,true};
    CHECK(xr_proc_pump_images(event->pid,&result)==XR_PROC_INVALID_ARGUMENT && result.drained && result.progressed);
    CHECK(xr_proc_close(event->pid)==-1);
    ForeignCall c={event->pid,XR_PROC_OK,{true,true},0};
    HANDLE thread=CreateThread(NULL,0,foreign_call,&c,0,NULL); CHECK(thread);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0); CHECK(CloseHandle(thread));
    CHECK(c.status==XR_PROC_INVALID_ARGUMENT && c.result.drained && c.result.progressed && c.closed==-1);
    (void)o;
}
static XrOsProcStatus observe_image(void *context, const XrProcImageEvent *event) {
    Observation *o=context;
    if (o->thread_checks && !o->checked_threads) {
        reentry_check(o,event);
        XrProcSpawnOptions options={0};
        options.memory=(XrProcMemory){o->resources,process_allocate,process_release,process_charge};
        options.new_process_group=true; options.image_mode=XR_PROC_IMAGES_WINDOWS_TREE;
        options.image_observer=(XrProcImageObserver){o,observe_image};
        XrProcId child=XR_PROC_INVALID; const char *args[]={executable,"--plain",NULL};
        CHECK(xr_proc_spawn(executable,args,&options,&child)==XR_PROC_INVALID_ARGUMENT && child==XR_PROC_INVALID);
        o->checked_threads=true;
    }
    if (o->exhaust) {
        /* A real consuming observer exhausts the shared work quota before the
         * cleanup phase. No ledger limit is mutated or refreshed. */
        volatile uint8_t scratch[256];
        for (size_t i=0;;++i) {
            if (xr_compile_resources_work(o->resources,1)!=XR_COMPILE_RESOURCE_OK) return XR_PROC_BUDGET;
            scratch[i%sizeof(scratch)]=(uint8_t)i;
        }
    }
    if (o->reject!=XR_PROC_OK) return o->reject;
    XrCompileResourceStatus rs=xr_compile_resources_work(o->resources,1);
    if (rs!=XR_COMPILE_RESOURCE_OK) return XR_PROC_BUDGET;
    BY_HANDLE_FILE_INFORMATION info;
    CHECK(GetFileInformationByHandle((HANDLE)event->file_handle,&info));
    void *memory=NULL;
    rs=xr_compile_resources_alloc(o->resources,sizeof(ImageRecord),&memory);
    if (rs!=XR_COMPILE_RESOURCE_OK) return rs==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_PROC_OUT_OF_MEMORY : XR_PROC_BUDGET;
    ImageRecord *r=memory;
    rs=xr_compile_resources_work(o->resources,sizeof(*r)+1);
    if (rs!=XR_COMPILE_RESOURCE_OK) { xr_compile_resources_free(r); return XR_PROC_BUDGET; }
    memset(r,0,sizeof(*r));
    if (!DuplicateHandle(GetCurrentProcess(),(HANDLE)event->file_handle,GetCurrentProcess(),&r->file,0,FALSE,DUPLICATE_SAME_ACCESS)) {
        xr_compile_resources_free(r); return xr_proc_last_error();
    }
    r->pid=(DWORD)event->pid; r->dll=event->kind==XR_PROC_IMAGE_DLL; r->next=o->images; o->images=r;
    if (r->dll) ++o->dlls; else ++o->executables;

    return XR_PROC_OK;
}
static void count_fixtures(Observation *o) {
    for (ImageRecord *r=o->images;r;r=r->next) {
        wchar_t name[32768]; DWORD n=GetFinalPathNameByHandleW(r->file,name,32768,FILE_NAME_NORMALIZED);
        CHECK(n && n<32768);
        if (wcsstr(name,L"image_observer_dll.dll")) ++o->fixture_loads;
    }
}
static void release_images(Observation *o) {
    while (o->images) { ImageRecord *r=o->images; o->images=r->next; CHECK(CloseHandle(r->file)); xr_compile_resources_free(r); }
}
static DWORD handle_count(void) { DWORD n=0; CHECK(GetProcessHandleCount(GetCurrentProcess(),&n)); return n; }
static XrProcessCancelled cancellation;
static void *cancellation_context;
static uint32_t command_timeout=5000;
static XrProcessStatus command(const char *mode, Observation *o, XrProcessResult *result) {
    XrProcessSpec spec;
    xtc_process_spec_init(&spec,executable,command_timeout); spec.cwd=directory;
    spec.argv[1]=mode; spec.argv[2]=dll_path;
    spec.image_mode=XR_PROC_IMAGES_WINDOWS_TREE;
    spec.completion_policy=command_completion;
    spec.image_observer=(XrProcImageObserver){o,observe_image};
    XrToolchainProcess *process=NULL;
    XrProcessStatus status=xtc_process_prepare(o->resources,&spec,&process);
    if (status==XTC_PROCESS_OK) status=xtc_process_run(process,cancellation,cancellation_context,result);
    xtc_process_free(process); return status;
}
static void simple(bool warmup) {
    XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    Observation o={0}; o.resources=r;
    XrProcessResult result={0}; DWORD before=handle_count();
    XrProcessStatus s=command("--load",&o,&result);
    count_fixtures(&o);
    fprintf(stderr,"trace status=%s exe=%u dll=%u fixture=%u\n",xtc_process_status_name(s),o.executables,o.dlls,o.fixture_loads);
    CHECK(s==XTC_PROCESS_OK && result.exit_code==0 && o.executables>=1 && o.fixture_loads==2);
    xtc_process_result_free(&result); release_images(&o);
    fprintf(stderr,"handle baseline=%lu after=%lu\n",before,handle_count());
    if (!warmup) CHECK(handle_count()==before);
    xr_compile_resources_release(r); CHECK(!physical && !allocation_count && !image_handle_count);
}

static void reset_faults(void) {
    CHECK(!physical && !allocation_count && !image_handle_count);
    calls=wait_calls=continue_calls=0;
    command_completion=XR_PROC_COMPLETE_TREE; fail_terminate=fail_root_continue=mismatch_accounting=false;
    exhaust_at_root=NULL; exhaust_work_limit=0;
    terminate_calls=root_exit_continues=child_exit_continues=0; pending_exit_pid=0;
    fail_at=fail_wait_at=fail_continue_at=SIZE_MAX;
    null_image=fail_assign=fail_resume=fail_accounting=fail_image_close=false;
    cancellation=NULL; cancellation_context=NULL; command_timeout=5000;
    injected_error=ERROR_READ_FAULT;
    persistent_wait=persistent_child_exit=false;
    root_pid=blocked_exit_pid=detached_exit_pid=0;
    event_count=first_chance_av=second_chance_av=0;
    sleep_ns=debug_wait_ns=pipe_ns=poll_ns=child_cpu_100ns=0; sleeps=idle_polls=0;
}
static XrProcessStatus trial(const char *mode, const XrCompileResourceLimits *limits, XrOsProcStatus reject,
    XrCompileResourceStats *out) {
    XrCompileResources *r=NULL;
    XrCompileResourceStatus rs=xr_compile_resources_new(limits,&r);
    if (rs!=XR_COMPILE_RESOURCE_OK) return rs==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XTC_PROCESS_OUT_OF_MEMORY : XTC_PROCESS_BUDGET;
    Observation o={0}; o.resources=r; o.reject=reject;
    XrProcessResult result={0}, sentinel=result;
    result.exit_code=1234; result.duration_ms=9876; sentinel=result;
    XrProcessStatus status=command(mode,&o,&result);
    if (status==XTC_PROCESS_OK) {
        CHECK(result.exit_code==0);
        xtc_process_result_free(&result);
    } else CHECK(!memcmp(&result,&sentinel,sizeof(result)));
    CHECK(xr_compile_resources_stats(r,out)==XR_COMPILE_RESOURCE_OK);
    CHECK(out->live_bytes==physical);
    release_images(&o);
    CHECK(allocation_count==1);
    xr_compile_resources_release(r);
    CHECK(!physical && !allocation_count && !image_handle_count);
    return status;
}
static void descendants_and_exceptions(unsigned count) {
    const char *modes[]={"--descendant","--grandchild","--exception","--breakpoint","--pipe-pressure","--event-burst"};
    CHECK(count<=sizeof(modes)/sizeof(*modes));
    for (unsigned i=0;i<count;++i) {
        reset_faults();
        XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
        Observation o={0}; o.resources=r; XrProcessResult result={0};
        ULONGLONG before=GetTickCount64(); uint64_t cpu=xr_time_process_cpu_ns();
        XrProcessStatus status=command(modes[i],&o,&result);
        fprintf(stderr,"fixture %s status=%s exit=%d exe=%u dll=%u\n",modes[i],xtc_process_status_name(status),result.exit_code,o.executables,o.dlls);
        fprintf(stderr,"events=%zu first-chance-av=%zu second-chance-av=%zu elapsed=%llu ms\n",event_count,first_chance_av,second_chance_av,(unsigned long long)(GetTickCount64()-before));
        if (profile_enabled) fprintf(stderr,"profile sleeps=%zu sleep-ms=%.3f idle-polls=%zu debug-wait-ms=%.3f pipe-ms=%.3f root-poll-ms=%.3f owner-cpu-ms=%.3f child-cpu-ms=%.3f\n",sleeps,sleep_ns/1e6,idle_polls,debug_wait_ns/1e6,pipe_ns/1e6,poll_ns/1e6,(xr_time_process_cpu_ns()-cpu)/1e6,child_cpu_100ns/1e4);
        if (i==5) CHECK(event_count>=300);
        if (status!=XTC_PROCESS_OK && result.stderr_bytes.data) fprintf(stderr,"%s\n",result.stderr_bytes.data);
        CHECK(status==XTC_PROCESS_OK && result.exit_code==0);
        count_fixtures(&o);
        if (i<2) CHECK(GetTickCount64()-before>=150 && o.fixture_loads==2 && o.executables>=i+2);
        if (i==4) CHECK(result.stdout_bytes.length==20000 && result.stderr_bytes.length==20000);
        xtc_process_result_free(&result); release_images(&o); xr_compile_resources_release(r);
        CHECK(!physical && !allocation_count && !image_handle_count);
    }
}
static bool cancel_events(void *context) { (void)context; return continue_calls>=2; }
static void root_executable_oracle(Observation *observation) {
    char console[32768]; DWORD length=GetSystemDirectoryA(console,sizeof(console));
    CHECK(length && length+sizeof("\\conhost.exe")<sizeof(console));
    memcpy(console+length,"\\conhost.exe",sizeof("\\conhost.exe"));
    HANDLE own=CreateFileA(executable,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,0,NULL);
    HANDLE host=CreateFileA(console,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,0,NULL);
    CHECK(own!=INVALID_HANDLE_VALUE && host!=INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION own_info,host_info;
    CHECK(GetFileInformationByHandle(own,&own_info) && GetFileInformationByHandle(host,&host_info));
    unsigned own_count=0,host_count=0,root_count=0;
    for (ImageRecord *image=observation->images;image;image=image->next) {
        if (image->dll) continue;
        BY_HANDLE_FILE_INFORMATION info; CHECK(GetFileInformationByHandle(image->file,&info));
        bool is_own=info.dwVolumeSerialNumber==own_info.dwVolumeSerialNumber &&
            info.nFileIndexHigh==own_info.nFileIndexHigh && info.nFileIndexLow==own_info.nFileIndexLow;
        bool is_host=info.dwVolumeSerialNumber==host_info.dwVolumeSerialNumber &&
            info.nFileIndexHigh==host_info.nFileIndexHigh && info.nFileIndexLow==host_info.nFileIndexLow;
        CHECK(is_own || is_host);
        if (is_own) { ++own_count; if (image->pid==root_pid) ++root_count; }
        else ++host_count;
    }
    CHECK(CloseHandle(own) && CloseHandle(host));
    CHECK(own_count==2 && root_count==1 && host_count<=1);
    CHECK(observation->executables==own_count+host_count);
    CHECK(root_exit_continues==1 && child_exit_continues+1==observation->executables && terminate_calls>=1);
}
static void root_completion(void) {
    const XrCompileResourceLimits finite={64 * 1024 * 1024,8 * 1024 * 1024,128000000};
    const char *modes[]={"--root-long-closed","--root-long-pipes","--root-nonzero"};
    for (unsigned i=0;i<3;++i) {
        reset_faults(); command_completion=XR_PROC_COMPLETE_ROOT;
        XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&finite,&r)==XR_COMPILE_RESOURCE_OK);
        Observation o={0}; o.resources=r; XrProcessResult result={0};
        CHECK(command(modes[i],&o,&result)==XTC_PROCESS_OK && result.exit_code==(i==2 ? 77 : 0));
        root_executable_oracle(&o);
        /* Native invocation rejects the nonzero root before publishing reports. */
        xtc_process_result_free(&result); release_images(&o); xr_compile_resources_release(r);
        CHECK(!physical && !allocation_count && !image_handle_count);
    }
    for (unsigned fault=0;fault<8;++fault) {
        reset_faults(); command_completion=XR_PROC_COMPLETE_ROOT;
        if (fault==0) fail_terminate=true;
        if (fault==1) fail_accounting=true;
        if (fault==2) fail_continue_at=0;
        if (fault==3) cancellation=cancel_events;
        if (fault==4) command_timeout=1;
        if (fault==6) fail_root_continue=true;
        if (fault==7) mismatch_accounting=true;
        XrCompileResourceStats used;
        XrProcessStatus expected=fault==3 ? XTC_PROCESS_CANCELLED : fault==4 ? XTC_PROCESS_TIMEOUT :
            (fault==5 || fault==7) ? XTC_PROCESS_UNSUPPORTED : XTC_PROCESS_IO;
        CHECK(trial("--root-long-pipes",&finite,fault==5 ? XR_PROC_UNSUPPORTED : XR_PROC_OK,&used)==expected);
    }
    reset_faults(); command_completion=XR_PROC_COMPLETE_ROOT;
    XrCompileResourceLimits small=finite; small.work=1000; XrCompileResourceStats used;
    CHECK(trial("--root-long-pipes",&small,XR_PROC_OK,&used)==XTC_PROCESS_BUDGET);
    reset_faults(); command_completion=XR_PROC_COMPLETE_ROOT;
    XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&finite,&r)==XR_COMPILE_RESOURCE_OK);
    Observation o={0}; o.resources=r; XrProcessResult result={0},sentinel=result;
    result.exit_code=1234; result.duration_ms=9876; sentinel=result;
    exhaust_at_root=r; exhaust_work_limit=finite.work;
    CHECK(command("--root-long-pipes",&o,&result)==XTC_PROCESS_BUDGET && !memcmp(&result,&sentinel,sizeof(result)));
    CHECK(root_exit_continues==1 && xr_compile_resources_stats(r,&used)==XR_COMPILE_RESOURCE_OK && used.work==finite.work);
    release_images(&o); xr_compile_resources_release(r); CHECK(!physical && !allocation_count && !image_handle_count);
    reset_faults(); puts("root completion: closed/inherited pipes, exits, terminate/accounting/Continue/cancel/timeout/observer/budget PASS");
}

static void failures(void) {
    XrCompileResourceStats out;
    for (unsigned i=0;i<5;++i) {
        reset_faults();
        if (i==0) null_image=true;
        if (i==1) fail_assign=true;
        if (i==2) fail_resume=true;
        if (i==3) fail_accounting=true;
        if (i==4) fail_image_close=true;
        CHECK(trial("--plain",&unlimited,XR_PROC_OK,&out)==(i==0 ? XTC_PROCESS_UNSUPPORTED : XTC_PROCESS_IO));
    }
    for (unsigned memory=0;memory<2;++memory) {
        for (unsigned which=0;which<2;++which) {
            for (size_t point=0;point<8;++point) {
                reset_faults(); injected_error=memory ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT;
                if (which) fail_continue_at=point; else fail_wait_at=point;
                CHECK(trial("--plain",&unlimited,XR_PROC_OK,&out)==(memory ? XTC_PROCESS_OUT_OF_MEMORY : XTC_PROCESS_IO));
            }
        }
    }
    const XrOsProcStatus rejects[]={XR_PROC_BUDGET,XR_PROC_OUT_OF_MEMORY,XR_PROC_IO,XR_PROC_UNSUPPORTED};
    const XrProcessStatus expected[]={XTC_PROCESS_BUDGET,XTC_PROCESS_OUT_OF_MEMORY,XTC_PROCESS_IO,XTC_PROCESS_UNSUPPORTED};
    for (unsigned i=0;i<4;++i) { reset_faults(); CHECK(trial("--plain",&unlimited,rejects[i],&out)==expected[i]); }
    reset_faults(); cancellation=cancel_events;
    CHECK(trial("--sleep",&unlimited,XR_PROC_OK,&out)==XTC_PROCESS_CANCELLED);
    reset_faults(); command_timeout=80;
    CHECK(trial("--sleep",&unlimited,XR_PROC_OK,&out)==XTC_PROCESS_TIMEOUT);
    reset_faults(); CHECK(trial("--plain",&unlimited,XR_PROC_OK,&out)==XTC_PROCESS_OK);
    size_t count=calls; printf("plain work=%llu peak=%llu\n",(unsigned long long)out.work,(unsigned long long)out.peak_bytes);
    for (size_t i=0;i<count;++i) {
        reset_faults(); fail_at=i;
        XrProcessStatus s=trial("--plain",&unlimited,XR_PROC_OK,&out);
        CHECK(s==XTC_PROCESS_OUT_OF_MEMORY && calls==i+1);
    }
    printf("image allocation failures: %zu; event IO/OOM: 32; startup/NULL/close/query: 5\n",count);
    const uint64_t work_limits[]={1,1000,4000,8000};
    for (unsigned i=0;i<sizeof(work_limits)/sizeof(*work_limits);++i) {
        reset_faults(); XrCompileResourceLimits small=unlimited; small.work=work_limits[i];
        CHECK(trial("--plain",&small,XR_PROC_OK,&out)==XTC_PROCESS_BUDGET);
        CHECK(out.work<=small.work);
    }
}

typedef struct PolicyReentry { XrCompileResources *resources; bool from_work, entered; } PolicyReentry;
static XrOsProcStatus accept_image(void *context,const XrProcImageEvent *event) { (void)context; (void)event; return XR_PROC_OK; }
static void policy_reenter(PolicyReentry *p) {
    p->entered=true;
    XrProcSpawnOptions nested={0}; nested.memory=xr_proc_system_memory();
    nested.complete_environment=true; nested.new_process_group=true;
    nested.image_mode=XR_PROC_IMAGES_WINDOWS_TREE; nested.image_observer.observe=accept_image;
    const char *args[]={executable,"--plain",NULL}; XrProcId pid=XR_PROC_INVALID;
    XrOsProcStatus status=xr_proc_spawn(executable,args,&nested,&pid);
    if (status==XR_PROC_OK) CHECK(xr_proc_close(pid)==0);
    CHECK(status==XR_PROC_INVALID_ARGUMENT && pid==XR_PROC_INVALID);
}
static XrOsProcStatus policy_allocate(void *context,size_t size,void **out) {
    PolicyReentry *p=context; if (!p->from_work && !p->entered) policy_reenter(p);
    return process_allocate(p->resources,size,out);
}
static void policy_release(void *context,void *memory) { PolicyReentry *p=context; process_release(p->resources,memory); }
static XrOsProcStatus policy_work(void *context,uint64_t units) {
    PolicyReentry *p=context; if (p->from_work && !p->entered) policy_reenter(p);
    return process_charge(p->resources,units);
}
static void policy_reentry(void) {
    for (unsigned kind=0;kind<2;++kind) {
        reset_faults(); XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
        PolicyReentry p={r,kind!=0,false}; XrProcSpawnOptions options={0};
        options.memory=(XrProcMemory){&p,policy_allocate,policy_release,policy_work};
        options.complete_environment=true; options.new_process_group=true;
        options.image_mode=XR_PROC_IMAGES_WINDOWS_TREE; options.image_observer.observe=accept_image;
        const char *args[]={executable,"--plain",NULL}; XrProcId pid=XR_PROC_INVALID;
        CHECK(xr_proc_spawn(executable,args,&options,&pid)==XR_PROC_OK && p.entered);
        CHECK(xr_proc_wait(pid,NULL)==-1);
        XrProcImagePumpResult pump={0}; ULONGLONG deadline=GetTickCount64()+5000;
        while (!pump.drained) { CHECK(GetTickCount64()<deadline); CHECK(xr_proc_pump_images(pid,&pump)==XR_PROC_OK); if (!pump.progressed) Sleep(1); }
        int code=-1; CHECK(xr_proc_try_wait(pid,&code)==XR_PROC_WAIT_EXITED && code==0);
        CHECK(xr_proc_close(pid)==0); xr_compile_resources_release(r);
        CHECK(!physical && !image_handle_count);
    }
}
static void boundaries_and_lifetime(void) {
    reset_faults(); XrCompileResourceStats baseline,used;
    CHECK(trial("--plain",&unlimited,XR_PROC_IO,&baseline)==XTC_PROCESS_IO);
    for (unsigned axis=0;axis<2;++axis) for (unsigned less=0;less<2;++less) {
        reset_faults(); XrCompileResourceLimits limits=unlimited;
        if (axis) limits.live_bytes=baseline.peak_bytes-less;
        else limits.allocated_bytes=baseline.allocated_bytes-less;
        CHECK(trial("--plain",&limits,XR_PROC_IO,&used)==(less ? XTC_PROCESS_BUDGET : XTC_PROCESS_IO));
    }
    reset_faults(); fail_continue_at=0;
    CHECK(trial("--plain",&unlimited,XR_PROC_BUDGET,&used)==XTC_PROCESS_BUDGET);
    reset_faults();
    XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    Observation o={0}; o.resources=r;
    XrProcessSpec spec; xtc_process_spec_init(&spec,executable,5000);
    spec.cwd=directory; spec.argv[1]="--load"; spec.argv[2]=dll_path;
    spec.image_mode=XR_PROC_IMAGES_WINDOWS_TREE; spec.image_observer=(XrProcImageObserver){&o,observe_image};
    XrToolchainProcess *process=NULL; CHECK(xtc_process_prepare(r,&spec,&process)==XTC_PROCESS_OK);
    memset(&spec,0xcd,sizeof(spec)); xr_compile_resources_release(r);
    XrProcessResult result={0}; CHECK(xtc_process_run(process,NULL,NULL,&result)==XTC_PROCESS_OK && result.exit_code==0);
    xtc_process_free(process); count_fixtures(&o); CHECK(o.fixture_loads==2);
    xtc_process_result_free(&result); release_images(&o); CHECK(!physical && !image_handle_count);
    reset_faults(); CHECK(trial("--debug-chain",&unlimited,XR_PROC_OK,&used)==XTC_PROCESS_UNSUPPORTED);
    puts("image exact/minus-one bytes, copied policy lifetime and real broken debug chain passed");
}

static XrOsProcStatus reject_image(void *context,const XrProcImageEvent *event) {
    (void)event; ++*(unsigned *)context; return XR_PROC_IO;
}
static void exact_dispatch_and_sticky_output(void) {
    /* One dispatch plus all 64 PID slots: this expected operation count is
     * independent of measured process timing and ledger statistics. */
    for (unsigned less=0;less<2;++less) {
        reset_faults(); XrCompileResourceLimits limits=unlimited; limits.work=1+65-less;
        XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&limits,&r)==XR_COMPILE_RESOURCE_OK);
        ProcDebug d={0}; d.memory=(XrProcMemory){r,process_allocate,process_release,process_charge};
        d.pids[0]=42; d.event.dwProcessId=42; d.event.dwDebugEventCode=CREATE_THREAD_DEBUG_EVENT;
        XrOsProcStatus expected=less ? XR_PROC_BUDGET : XR_PROC_OK;
        CHECK(proc_debug_dispatch(&d,false)==expected);
        CHECK(d.dispatched==!less && d.pids[0]==42);
        XrCompileResourceStats used; CHECK(xr_compile_resources_stats(r,&used)==XR_COMPILE_RESOURCE_OK);
        CHECK(used.work==(less ? 1 : 66));
        CHECK(proc_debug_dispatch(&d,false)==expected);
        CHECK(xr_compile_resources_stats(r,&used)==XR_COMPILE_RESOURCE_OK && used.work==(less ? 1 : 66));
        xr_compile_resources_release(r); CHECK(!physical);
    }
    reset_faults(); XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    unsigned observed=0; XrProcSpawnOptions options={0};
    options.memory=(XrProcMemory){r,process_allocate,process_release,process_charge};
    options.complete_environment=true; options.new_process_group=true; options.image_mode=XR_PROC_IMAGES_WINDOWS_TREE;
    options.image_observer=(XrProcImageObserver){&observed,reject_image};
    const char *args[]={executable,"--plain",NULL}; XrProcId pid=XR_PROC_INVALID;
    CHECK(xr_proc_spawn(executable,args,&options,&pid)==XR_PROC_OK);
    XrProcImagePumpResult result,before; XrOsProcStatus status; ULONGLONG deadline=GetTickCount64()+5000;
    do {
        CHECK(GetTickCount64()<deadline); memset(&result,0xa5,sizeof(result)); memcpy(&before,&result,sizeof(result));
        status=xr_proc_pump_images(pid,&result); if (status==XR_PROC_OK && !result.progressed) Sleep(1);
    } while (status==XR_PROC_OK);
    CHECK(status==XR_PROC_IO && !memcmp(&result,&before,sizeof(result)) && observed==1);
    XrCompileResourceStats used,after; CHECK(xr_compile_resources_stats(r,&used)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_proc_pump_images(pid,&result)==XR_PROC_IO && !memcmp(&result,&before,sizeof(result)) && observed==1);
    CHECK(xr_proc_close(pid)==0);
    CHECK(xr_compile_resources_stats(r,&after)==XR_COMPILE_RESOURCE_OK && used.work==after.work);
    xr_compile_resources_release(r); CHECK(!physical && !image_handle_count);
    puts("dispatch formula=65; sticky output and cleanup without new work passed");
}

static void persistent_cleanup(void) {
    for (unsigned child=0;child<2;++child) {
        reset_faults(); XrCompileResourceStats used; DWORD before=handle_count();
        persistent_wait=child==0; persistent_child_exit=child!=0;
        ULONGLONG start=GetTickCount64();
        CHECK(trial(child ? "--descendant" : "--plain",&unlimited,XR_PROC_OK,&used)==XTC_PROCESS_IO);
        ULONGLONG elapsed=GetTickCount64()-start;
        CHECK(elapsed>=5000 && elapsed<12000);
        if (child) CHECK(blocked_exit_pid && blocked_exit_pid==detached_exit_pid);
        CHECK(handle_count()==before);
    }
    reset_faults(); puts("persistent wait/child-exit Continue failures reached bounded cleanup fallback");
}

static void thread_and_budget(void) {
    reset_faults();
    XrCompileResources *r=NULL; CHECK(xr_compile_resources_new(&unlimited,&r)==XR_COMPILE_RESOURCE_OK);
    Observation o={0}; o.resources=r; o.thread_checks=true; XrProcessResult result={0};
    CHECK(command("--plain",&o,&result)==XTC_PROCESS_OK && o.checked_threads);
    xtc_process_result_free(&result); release_images(&o); xr_compile_resources_release(r);
    CHECK(!physical && !image_handle_count);
    reset_faults(); XrCompileResourceLimits limits=unlimited; limits.work=100000;
    r=NULL; CHECK(xr_compile_resources_new(&limits,&r)==XR_COMPILE_RESOURCE_OK);
    o=(Observation){0}; o.resources=r; o.exhaust=true;
    CHECK(command("--plain",&o,&result)==XTC_PROCESS_BUDGET && !result.stdout_bytes.data);
    XrCompileResourceStats used; CHECK(xr_compile_resources_stats(r,&used)==XR_COMPILE_RESOURCE_OK && used.work==limits.work);
    release_images(&o); xr_compile_resources_release(r); CHECK(!physical && !image_handle_count);
}
static int load_twice(const char *path) {
    for (unsigned i=0;i<2;++i) { HMODULE h=LoadLibraryA(path); if (!h) return 2; if (!FreeLibrary(h)) return 3; }
    return 0;
}
static int child_spawn(const char *mode,const char *dll) {
    char self[32768],cmd[65536]; CHECK(GetModuleFileNameA(NULL,self,sizeof(self)));
    CHECK(snprintf(cmd,sizeof(cmd),"\"%s\" %s \"%s\"",self,mode,dll)>0);
    STARTUPINFOA startup={0}; startup.cb=sizeof(startup);
    PROCESS_INFORMATION child={0};
    if (!CreateProcessA(self,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&child)) return 10;
    CloseHandle(child.hThread); CloseHandle(child.hProcess); return 0;
}
/* A debuggee starting its own DEBUG_PROCESS tree really interrupts inherited
 * debug delivery. The production owner must reject the resulting job mismatch. */
static int debug_chain(const char *dll) {
    char self[32768],cmd[65536]; CHECK(GetModuleFileNameA(NULL,self,sizeof(self)));
    CHECK(snprintf(cmd,sizeof(cmd),"\"%s\" --load \"%s\"",self,dll)>0);
    STARTUPINFOA startup={0}; startup.cb=sizeof(startup); PROCESS_INFORMATION child={0};
    if (!CreateProcessA(self,cmd,NULL,NULL,TRUE,DEBUG_PROCESS|CREATE_NO_WINDOW,NULL,NULL,&startup,&child)) return 70;
    unsigned live=0; bool root_exit=false;
    while (!root_exit || live) {
        DEBUG_EVENT event; if (!WaitForDebugEvent(&event,3000)) return 71;
        DWORD disposition=DBG_CONTINUE;
        if (event.dwDebugEventCode==CREATE_PROCESS_DEBUG_EVENT) {
            ++live; if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
        } else if (event.dwDebugEventCode==LOAD_DLL_DEBUG_EVENT) {
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
        } else if (event.dwDebugEventCode==EXIT_PROCESS_DEBUG_EVENT) {
            --live; if (event.dwProcessId==child.dwProcessId) root_exit=true;
        } else if (event.dwDebugEventCode==EXCEPTION_DEBUG_EVENT &&
            event.u.Exception.ExceptionRecord.ExceptionCode!=EXCEPTION_BREAKPOINT) disposition=DBG_EXCEPTION_NOT_HANDLED;
        if (!ContinueDebugEvent(event.dwProcessId,event.dwThreadId,disposition)) return 72;
    }
    DWORD code=1; CHECK(WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess,&code));
    CHECK(CloseHandle(child.hThread) && CloseHandle(child.hProcess)); return code ? 73 : 0;
}
static int handled_exception(bool breakpoint) {
    __try { if (breakpoint) DebugBreak(); else RaiseException(0xe0424242u,0,0,NULL); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 31;
}
int main(int argc,char **argv) {
    if (argc>1 && !strcmp(argv[1],"--load")) {
        return load_twice(argv[2]);
    }
    if (argc>1 && !strcmp(argv[1],"--root-long-closed")) return child_spawn("--long-closed",argv[2]);
    if (argc>1 && !strcmp(argv[1],"--root-long-pipes")) return child_spawn("--sleep",argv[2]);
    if (argc>1 && !strcmp(argv[1],"--root-nonzero")) {
        int status=child_spawn("--sleep",argv[2]); return status ? status : 77;
    }
    if (argc>1 && !strcmp(argv[1],"--long-closed")) {
        HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE),err=GetStdHandle(STD_ERROR_HANDLE);
        if (out && out!=INVALID_HANDLE_VALUE) CloseHandle(out);
        if (err && err!=INVALID_HANDLE_VALUE && err!=out) CloseHandle(err);
        SetStdHandle(STD_OUTPUT_HANDLE,NULL); SetStdHandle(STD_ERROR_HANDLE,NULL);
        Sleep(10000); return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--plain")) return 0;
    if (argc>1 && !strcmp(argv[1],"--sleep")) { Sleep(10000); return 0; }
    if (argc>1 && !strcmp(argv[1],"--descendant")) return child_spawn("--delayed",argv[2]);
    if (argc>1 && !strcmp(argv[1],"--grandchild")) return child_spawn("--descendant",argv[2]);
    if (argc>1 && !strcmp(argv[1],"--delayed")) {
        HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE),err=GetStdHandle(STD_ERROR_HANDLE);
        if (out && out!=INVALID_HANDLE_VALUE) CloseHandle(out);
        if (err && err!=INVALID_HANDLE_VALUE && err!=out) CloseHandle(err);
        SetStdHandle(STD_OUTPUT_HANDLE,NULL); SetStdHandle(STD_ERROR_HANDLE,NULL);
        Sleep(180); return load_twice(argv[2]);
    }
    if (argc>1 && !strcmp(argv[1],"--debug-chain")) return debug_chain(argv[2]);
    if (argc>1 && !strcmp(argv[1],"--event-burst")) {
        for (unsigned i=0;i<300;++i) if (handled_exception(false)) return 32;
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--exception")) return handled_exception(false);
    if (argc>1 && !strcmp(argv[1],"--breakpoint")) return handled_exception(true);
    if (argc>1 && !strcmp(argv[1],"--pipe-pressure")) {
        for (unsigned i=0;i<20000;++i) { putchar('a'); fputc('b',stderr); } return 0;
    }
    CHECK(GetModuleFileNameA(NULL,executable,sizeof(executable)));
    CHECK(GetCurrentDirectoryA(sizeof(directory),directory));
    bool descendants_only=argc==3 && !strcmp(argv[1],"--descendant-gate");
    CHECK((argc==2 || descendants_only) && strlen(argv[descendants_only ? 2 : 1])<sizeof(dll_path));
    strcpy(dll_path,argv[descendants_only ? 2 : 1]);
    if (descendants_only) { profile_enabled=true; descendants_and_exceptions(2); return 0; }
    simple(true); simple(false); descendants_and_exceptions(6); failures(); thread_and_budget(); policy_reentry(); boundaries_and_lifetime(); exact_dispatch_and_sticky_output(); persistent_cleanup(); root_completion(); puts("image observer passed"); return 0;
}
