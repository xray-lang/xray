/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_pipe_owned.c - Owned byte I/O, real child stdio and cancellation recovery
 */
#include "os/os_pipe_owned.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <limits.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"owned pipe %d: %s\n",__LINE__,#c);exit(1);}}while(0)

#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <wchar.h>

static DWORD caller_thread;
static size_t live_blocks;
static bool fail_allocate,misalign_allocate,current=true;
static uint8_t *misaligned_memory;
static size_t fail_size;
static _Atomic(bool) gate_read,partial_write,fail_pipe_close,fail_cancel;
static HANDLE entered,proceed;

static XrOsIoStatus test_allocate(void *context,size_t bytes,void **output){
 CHECK(context&&GetCurrentThreadId()==caller_thread);
 if(fail_allocate||(fail_size&&bytes==fail_size)){fail_allocate=false;fail_size=0;return XR_OS_IO_OUT_OF_MEMORY;}
 void *memory=xr_malloc(bytes+(misalign_allocate?1u:0u));if(!memory)return XR_OS_IO_OUT_OF_MEMORY;
 if(misalign_allocate){misalign_allocate=false;misaligned_memory=memory;memory=misaligned_memory+1;}
 ++live_blocks;*output=memory;return XR_OS_IO_OK;
}
static void test_free(void *context,void *memory){
 CHECK(context&&GetCurrentThreadId()==caller_thread&&memory&&live_blocks);--live_blocks;
 if(misaligned_memory&&memory==misaligned_memory+1){memory=misaligned_memory;misaligned_memory=NULL;}
 xr_free(memory);
}
static bool test_current(void *context){CHECK(context&&GetCurrentThreadId()==caller_thread);return current;}
static BOOL WINAPI test_read(HANDLE handle,LPVOID buffer,DWORD length,LPDWORD count,LPOVERLAPPED overlap){
 if(atomic_exchange_explicit(&gate_read,false,memory_order_acq_rel)){
  CHECK(SetEvent(entered));CHECK(WaitForSingleObject(proceed,30000)==WAIT_OBJECT_0);
 }
 return ReadFile(handle,buffer,length,count,overlap);
}
static BOOL WINAPI test_write(HANDLE handle,LPCVOID buffer,DWORD length,LPDWORD count,LPOVERLAPPED overlap){
 if(atomic_exchange_explicit(&partial_write,false,memory_order_acq_rel))length/=2;
 return WriteFile(handle,buffer,length,count,overlap);
}
static BOOL WINAPI test_close(HANDLE handle){
 if(GetFileType(handle)==FILE_TYPE_PIPE&&atomic_exchange_explicit(&fail_pipe_close,false,memory_order_acq_rel)){
  SetLastError(ERROR_ACCESS_DENIED);return FALSE;
 }
 return CloseHandle(handle);
}
static BOOL WINAPI test_cancel(HANDLE handle){
 if(atomic_exchange_explicit(&fail_cancel,false,memory_order_acq_rel)){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
 return CancelSynchronousIo(handle);
}
#define ReadFile test_read
#define WriteFile test_write
#define CloseHandle test_close
#define CancelSynchronousIo test_cancel
#include "os/win/pipe_owned_win.c"
#undef ReadFile
#undef WriteFile
#undef CloseHandle
#undef CancelSynchronousIo

static XrOsIoPolicy memory={&live_blocks,test_allocate,test_free};
static XrOsPipeQuery query={&current,test_current};
static void bytes_fill(uint8_t data[53]){for(unsigned n=0;n<53;++n)data[n]=(uint8_t)(n*37u);data[0]=0;data[17]=255;}
static XrOsPipeProgress finish(XrOsPipeOperation *operation){
 ULONGLONG end=GetTickCount64()+30000;XrOsPipeProgress progress={0};
 do{CHECK(xr_os_io_pipe_poll(operation,&progress)==XR_OS_IO_OK);if(progress.terminal)return progress;Sleep(1);}while(GetTickCount64()<end);
 CHECK(false);return progress;
}
static void close_operation(XrOsPipeOperation *operation){
 ULONGLONG end=GetTickCount64()+30000;bool closed=false;
 do{CHECK(xr_os_io_pipe_operation_close(operation,&closed)==XR_OS_IO_OK);if(closed)return;Sleep(1);}while(GetTickCount64()<end);
 CHECK(false);
}
static void open_pipe(XrOsPipeOwner *owner){bool opened=false;CHECK(xr_os_io_pipe_open(&memory,&query,owner,&opened)==XR_OS_IO_OK&&opened);}
static void dispose_pipe(XrOsPipeOwner *owner){bool closed=false;CHECK(xr_os_io_pipe_dispose(owner,&closed)==XR_OS_IO_OK&&closed);}
static void close_end(XrOsPipeOwner *owner,XrOsPipeEnd end){bool closed=false;CHECK(xr_os_io_pipe_close_end(owner,end,&closed)==XR_OS_IO_OK&&closed);}

static void byte_operations(bool partial){
 XrOsPipeOwner owner={0};open_pipe(&owner);uint8_t source[53],expected[53];bytes_fill(source);memcpy(expected,source,sizeof(source));
 XrOsPipeOperation read={0},write={0};XrOsPipeRequest reading={XR_OS_PIPE_READ,NULL,128},writing={XR_OS_PIPE_WRITE,source,53};
 CHECK(xr_os_io_pipe_begin(&owner,&reading,&query,&read)==XR_OS_IO_OK);
 atomic_store_explicit(&partial_write,partial,memory_order_release);
 CHECK(xr_os_io_pipe_begin(&owner,&writing,&query,&write)==XR_OS_IO_OK);memset(source,0,sizeof(source));
 XrOsPipeProgress sent=finish(&write),received=finish(&read);size_t count=partial?26u:53u;
 CHECK(sent.status==XR_OS_IO_OK&&sent.count==count&&received.status==XR_OS_IO_OK&&received.count==count&&!memcmp(received.bytes,expected,count));
 close_operation(&read);close_operation(&write);close_end(&owner,XR_OS_PIPE_WRITE);
 CHECK(xr_os_io_pipe_begin(&owner,&reading,&query,&read)==XR_OS_IO_OK);
 XrOsPipeProgress eof=finish(&read);CHECK(eof.status==XR_OS_IO_OK&&!eof.count);close_operation(&read);dispose_pipe(&owner);CHECK(!live_blocks);
}
static void cancellation_and_close(void){
 XrOsPipeOwner owner={0};open_pipe(&owner);XrOsPipeOperation operation={0};XrOsPipeRequest reading={XR_OS_PIPE_READ,NULL,128};
 current=false;CHECK(xr_os_io_pipe_begin(&owner,&reading,&query,&operation)==XR_OS_IO_CANCELLED);current=true;
 entered=CreateEventW(NULL,TRUE,FALSE,NULL);proceed=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(entered&&proceed);
 atomic_store_explicit(&gate_read,true,memory_order_release);
 CHECK(xr_os_io_pipe_begin(&owner,&reading,&query,&operation)==XR_OS_IO_OK);
 CHECK(WaitForSingleObject(entered,30000)==WAIT_OBJECT_0);
 bool closed=true;CHECK(xr_os_io_pipe_close_end(&owner,XR_OS_PIPE_READ,&closed)==XR_OS_IO_OK&&!closed);
 closed=true;CHECK(xr_os_io_pipe_dispose(&owner,&closed)==XR_OS_IO_OK&&!closed);
 atomic_store_explicit(&fail_cancel,true,memory_order_release);
 CHECK(xr_os_io_pipe_cancel(&operation)==XR_OS_IO_IO);
 CHECK(xr_os_io_pipe_cancel(&operation)==XR_OS_IO_OK);
 CHECK(SetEvent(proceed));XrOsPipeProgress result=finish(&operation);
 CHECK(result.status==XR_OS_IO_CANCELLED&&result.native_error==ERROR_OPERATION_ABORTED&&!result.count);
 atomic_store_explicit(&fail_pipe_close,true,memory_order_release);closed=true;
 CHECK(xr_os_io_pipe_operation_close(&operation,&closed)==XR_OS_IO_IO&&closed);
 close_operation(&operation);CHECK(CloseHandle(entered)&&CloseHandle(proceed));entered=proceed=NULL;
 atomic_store_explicit(&fail_pipe_close,true,memory_order_release);closed=false;
 CHECK(xr_os_io_pipe_close_end(&owner,XR_OS_PIPE_WRITE,&closed)==XR_OS_IO_IO&&!closed);
 close_end(&owner,XR_OS_PIPE_WRITE);dispose_pipe(&owner);CHECK(!live_blocks);
}
static void failed_preparation(void){
 XrOsPipeOwner owner={0};bool opened=true;fail_allocate=true;
 CHECK(xr_os_io_pipe_open(&memory,&query,&owner,&opened)==XR_OS_IO_OUT_OF_MEMORY&&opened);dispose_pipe(&owner);
 misalign_allocate=true;CHECK(xr_os_io_pipe_open(&memory,&query,&owner,&opened)==XR_OS_IO_BAD_ARGUMENT&&opened&&!live_blocks);
 open_pipe(&owner);uint8_t bytes[53];bytes_fill(bytes);XrOsPipeOperation operation={0};XrOsPipeRequest request={XR_OS_PIPE_WRITE,bytes,53};
 misalign_allocate=true;CHECK(xr_os_io_pipe_begin(&owner,&request,&query,&operation)==XR_OS_IO_BAD_ARGUMENT);
 fail_size=53;CHECK(xr_os_io_pipe_begin(&owner,&request,&query,&operation)==XR_OS_IO_OUT_OF_MEMORY);close_operation(&operation);
 XrOsPipeOwner copy=owner;intptr_t borrowed=7;
 CHECK(xr_os_io_pipe_borrow(&copy,XR_OS_PIPE_READ,&borrowed)==XR_OS_IO_BAD_ARGUMENT&&borrowed==7);
 CHECK(xr_os_io_pipe_borrow(&owner,(XrOsPipeEnd)-1,&borrowed)==XR_OS_IO_BAD_ARGUMENT&&borrowed==7);
 request.end=(XrOsPipeEnd)-1;CHECK(xr_os_io_pipe_begin(&owner,&request,&query,&operation)==XR_OS_IO_BAD_ARGUMENT);
 dispose_pipe(&owner);CHECK(!live_blocks);
}

typedef struct PipeChild { HANDLE process,job; } PipeChild;
static bool spawn_child(XrOsPipeOwner *owner,bool writer,PipeChild *child){
 intptr_t raw=0;HANDLE redirect=NULL,null=NULL,job=NULL;STARTUPINFOEXW startup={0};PROCESS_INFORMATION process={0};
 LPPROC_THREAD_ATTRIBUTE_LIST attributes=NULL;SIZE_T bytes=0;bool initialized=false,okay=false;
 wchar_t path[32768],command[32768];DWORD count=GetModuleFileNameW(NULL,path,32768);
 if(!count||count>=32768||swprintf_s(command,32768,L"\"%ls\" --child-%ls",path,owner?(writer?L"write":L"read"):L"neutral")<0)goto done;
 if(owner){
  if(xr_os_io_pipe_borrow(owner,writer?XR_OS_PIPE_WRITE:XR_OS_PIPE_READ,&raw)!=XR_OS_IO_OK)goto done;
  if(!DuplicateHandle(GetCurrentProcess(),(HANDLE)raw,GetCurrentProcess(),&redirect,0,TRUE,DUPLICATE_SAME_ACCESS))goto done;
 }
 SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
 null=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,NULL);
 if(null==INVALID_HANDLE_VALUE){null=NULL;goto done;}
 if(!owner&&!DuplicateHandle(GetCurrentProcess(),null,GetCurrentProcess(),&redirect,0,TRUE,DUPLICATE_SAME_ACCESS))goto done;
 job=CreateJobObjectW(NULL,NULL);if(!job)goto done;JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
 limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))goto done;
 (void)InitializeProcThreadAttributeList(NULL,1,0,&bytes);attributes=xr_malloc(bytes);if(!attributes)goto done;
 if(!InitializeProcThreadAttributeList(attributes,1,0,&bytes))goto done;initialized=true;
 HANDLE inherited[2]={redirect,null};if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),NULL,NULL))goto done;
 startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
 startup.StartupInfo.hStdInput=writer?null:redirect;startup.StartupInfo.hStdOutput=writer?redirect:null;startup.StartupInfo.hStdError=null;startup.lpAttributeList=attributes;
 if(!CreateProcessW(path,command,NULL,NULL,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,NULL,NULL,&startup.StartupInfo,&process))goto done;
 if(!AssignProcessToJobObject(job,process.hProcess)||ResumeThread(process.hThread)==(DWORD)-1)goto done;
 child->process=process.hProcess;child->job=job;process.hProcess=NULL;job=NULL;okay=true;
done:
 if(process.hProcess){(void)TerminateProcess(process.hProcess,1);(void)WaitForSingleObject(process.hProcess,30000);CloseHandle(process.hProcess);}
 if(process.hThread)CloseHandle(process.hThread);
 if(job)CloseHandle(job);if(redirect)CloseHandle(redirect);if(null)CloseHandle(null);
 if(initialized)DeleteProcThreadAttributeList(attributes);xr_free(attributes);return okay;
}
static void join_child(PipeChild *child){
 DWORD waited=WaitForSingleObject(child->process,30000),code=1;
 if(waited!=WAIT_OBJECT_0){CHECK(TerminateJobObject(child->job,1));CHECK(WaitForSingleObject(child->process,30000)==WAIT_OBJECT_0);}
 CHECK(GetExitCodeProcess(child->process,&code));CHECK(CloseHandle(child->process)&&CloseHandle(child->job));
 CHECK(waited==WAIT_OBJECT_0&&code==0);memset(child,0,sizeof(*child));
}
/* Win32 pipe and process services retain their own process-lifetime handles.
 * Establish those services without exercising an owned pipe or policy. */
static void native_services_baseline(void){
 HANDLE reading=NULL,writing=NULL;CHECK(CreatePipe(&reading,&writing,NULL,0));
 CHECK(CloseHandle(reading)&&CloseHandle(writing));
 PipeChild child={0};CHECK(spawn_child(NULL,false,&child));join_child(&child);
 CHECK(!live_blocks);
}
static int child_main(bool writer){
 uint8_t expected[53];bytes_fill(expected);
 CHECK(_setmode(_fileno(stdin),_O_BINARY)!=-1&&_setmode(_fileno(stdout),_O_BINARY)!=-1);
 if(writer){CHECK(fwrite(expected,1,sizeof(expected),stdout)==sizeof(expected)&&!fflush(stdout));return 0;}
 uint8_t buffer[128];size_t total=0,count=0;
 while((count=fread(buffer,1,sizeof(buffer),stdin))!=0){CHECK(total<=sizeof(expected)&&count<=sizeof(expected)-total&&!memcmp(buffer,expected+total,count));total+=count;}
 CHECK(!ferror(stdin)&&total==sizeof(expected));return 0;
}
static void child_stdio(bool writer){
 XrOsPipeOwner owner={0};open_pipe(&owner);PipeChild child={0};CHECK(spawn_child(&owner,writer,&child));
 close_end(&owner,writer?XR_OS_PIPE_WRITE:XR_OS_PIPE_READ);
 uint8_t data[53],expected[53];bytes_fill(data);memcpy(expected,data,sizeof(data));XrOsPipeOperation operation={0};
 XrOsPipeRequest request={writer?XR_OS_PIPE_READ:XR_OS_PIPE_WRITE,writer?NULL:data,writer?128u:53u};
 CHECK(xr_os_io_pipe_begin(&owner,&request,&query,&operation)==XR_OS_IO_OK);memset(data,0,sizeof(data));
 XrOsPipeProgress result=finish(&operation);CHECK(result.status==XR_OS_IO_OK&&result.count==53);
 if(writer)CHECK(!memcmp(result.bytes,expected,53));close_operation(&operation);
 if(!writer)close_end(&owner,XR_OS_PIPE_WRITE);
 join_child(&child);
 if(writer){CHECK(xr_os_io_pipe_begin(&owner,&request,&query,&operation)==XR_OS_IO_OK);result=finish(&operation);CHECK(result.status==XR_OS_IO_OK&&!result.count);close_operation(&operation);}
 dispose_pipe(&owner);CHECK(!live_blocks);
}
#else
#include "os/unix/pipe_owned_unix.c"
#include "pipe_probe_posix_cases.h"
#endif

int main(int argc,char **argv){
#if defined(XR_OS_WINDOWS)
 if(argc==2&&!strcmp(argv[1],"--child-neutral"))return 0;
 if(argc==2&&!strcmp(argv[1],"--child-write"))return child_main(true);
 if(argc==2&&!strcmp(argv[1],"--child-read"))return child_main(false);
 CHECK(argc==1);caller_thread=GetCurrentThreadId();native_services_baseline();
 DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
 failed_preparation();byte_operations(false);byte_operations(true);cancellation_and_close();child_stdio(true);child_stdio(false);
 CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before&&!live_blocks);
#else
 (void)argc;(void)argv;XrOsPipeOwner owner={0};bool opened=true;
 CHECK(xr_os_io_pipe_open(NULL,NULL,&owner,&opened)==XR_OS_IO_UNSUPPORTED&&opened&&!owner.state);
 pipe_probe_closed_tail();
#endif
 puts("42");return 0;
}
