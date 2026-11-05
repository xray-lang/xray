/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_runtime.h - Real host providers under counted runtime ownership
 */
#ifndef HOST_OS_RUNTIME_H
#define HOST_OS_RUNTIME_H
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_native_callable.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include "os/os_fs.h"
#include "os/os_thread.h"
#include "os/os_system.h"
#include "os/os_file_metadata.h"
#include "os/os_file_mutation.h"
#include "os/os_thread_token.h"
#include "os/os_file_links.h"
#include "os/os_file_temp.h"
#include "os/os_directory_owned.h"
#if defined(XR_OS_WINDOWS)
#include <windows.h>
#ifdef interface
#undef interface
#endif
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"host OS %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static XrOsIoStatus host_system_uid(XrOsOptionalI64 *);
static XrOsIoStatus host_system_gid(XrOsOptionalI64 *);
static XrOsIoStatus host_system_ppid(XrOsSystemOwner *,void *,bool(*)(void *),int64_t *);
static XrOsIoStatus host_system_kill(XrOsSystemOwner *,int64_t,int64_t,void *,bool(*)(void *),bool *);
static XrOsIoStatus host_system_total(int64_t *);
static XrOsIoStatus host_system_free(XrOsSystemOwner *,int64_t *);
static XrOsIoStatus host_system_uptime(double *);
#if !defined(XR_OS_WINDOWS)
static XrOsIoStatus host_system_loadavg(XrOsLoadAverages *);
#endif
static XrOsIoStatus host_system_close(XrOsSystemOwner *);
static XrOsIoStatus host_metadata(const XrOsIoPolicy *,const char *,size_t,bool(*)(void *),XrOsFileMetadataOwner *,XrOsFileMetadata *);
static XrOsIoStatus host_metadata_close(XrOsFileMetadataOwner *);
static XrOsIoStatus host_io_mkdir(const XrOsIoPolicy *,XrOsPathSpan,bool(*)(void *),bool *);
static XrOsIoStatus host_io_rmdir(const XrOsIoPolicy *,XrOsPathSpan,bool(*)(void *),bool *);
static XrOsIoStatus host_io_remove(const XrOsIoPolicy *,XrOsPathSpan,bool(*)(void *),XrOsFileMetadataOwner *,bool *);
static XrOsIoStatus host_io_rename(const XrOsIoPolicy *,XrOsPathSpan,XrOsPathSpan,bool(*)(void *),bool *);
static XrOsIoStatus host_io_utime(const XrOsIoPolicy *,XrOsPathSpan,bool(*)(void *),XrOsFileMetadataOwner *,bool *);
static XrOsIoStatus host_links_symlink(const XrOsIoPolicy *,XrOsPathSpan,XrOsPathSpan,bool(*)(void *),bool *);
static XrOsIoStatus host_links_readlink(const XrOsIoPolicy *,XrOsPathSpan,bool(*)(void *),XrOsFileMetadataOwner *,XrOsFileLinkText *);
static XrOsIoStatus host_links_realpath(const XrOsIoPolicy *,XrOsPathSpan,bool(*)(void *),XrOsFileMetadataOwner *,XrOsFileLinkText *);
static XrOsIoStatus host_temp_directory(const XrOsFileTempRequest *,XrOsFileTempOwner *,bool *);
static XrOsIoStatus host_temp_file(const XrOsFileTempRequest *,XrOsFileTempOwner *,bool *);
static XrOsIoStatus host_temp_finish(const XrOsIoPolicy *,XrOsFileTempOwner *,bool);
static XrOsIoStatus host_directory_open(const XrOsDirectoryRequest *,XrOsDirectoryOwner *,bool *);
static XrOsIoStatus host_directory_next(XrOsDirectoryOwner *,void *,bool(*)(void *),XrOsDirectoryText *);
static XrOsIoStatus host_directory_close(XrOsDirectoryOwner *);
static unsigned int host_count(void);
static int host_pin(unsigned int);
static XrOsIoStatus host_chmod(const XrOsIoPolicy *,const char *,int64_t,bool(*)(void *),bool *);
static XrOsIoStatus host_chdir(const XrOsIoPolicy *,const char *,bool(*)(void *),bool *);
static XrOsIoStatus host_token_current(const XrOsThreadTokenQuery *,int64_t *);
static XrOsIoStatus host_token_alive(const XrOsThreadTokenQuery *,int64_t,bool *);
#define xr_os_io_thread_token_current host_token_current
#define xr_os_io_thread_token_alive host_token_alive
#define xr_os_io_path_mkdir host_io_mkdir
#define xr_os_io_path_rmdir host_io_rmdir
#define xr_os_io_path_remove host_io_remove
#define xr_os_io_path_rename host_io_rename
#define xr_os_io_path_utime_now host_io_utime
#define xr_os_io_path_symlink host_links_symlink
#define xr_os_io_path_readlink host_links_readlink
#define xr_os_io_path_realpath host_links_realpath
#define xr_os_io_temp_directory host_temp_directory
#define xr_os_io_temp_file host_temp_file
#define xr_os_io_temp_finish host_temp_finish
#define xr_os_io_directory_open host_directory_open
#define xr_os_io_directory_next host_directory_next
#define xr_os_io_directory_close host_directory_close
#define xr_os_io_file_metadata host_metadata
#define xr_os_io_file_metadata_close host_metadata_close
#define xr_os_cpu_count host_count
#define xr_thread_pin_to_cpu host_pin
#define xr_os_io_chmod host_chmod
#define xr_os_io_chdir host_chdir
#define xr_os_io_uid host_system_uid
#define xr_os_io_gid host_system_gid
#define xr_os_io_ppid host_system_ppid
#define xr_os_io_kill host_system_kill
#define xr_os_io_total_memory host_system_total
#define xr_os_io_free_memory host_system_free
#define xr_os_io_uptime host_system_uptime
#define xr_os_io_loadavg host_system_loadavg
#define xr_os_io_system_close host_system_close
#include "../xir_runtime_allocations.h"
#undef xr_os_io_thread_token_current
#undef xr_os_io_thread_token_alive
#undef xr_os_io_uid
#undef xr_os_io_gid
#undef xr_os_io_ppid
#undef xr_os_io_kill
#undef xr_os_io_total_memory
#undef xr_os_io_free_memory
#undef xr_os_io_uptime
#undef xr_os_io_loadavg
#undef xr_os_io_system_close

#undef xr_os_io_path_mkdir
#undef xr_os_io_path_rmdir
#undef xr_os_io_path_remove
#undef xr_os_io_path_rename
#undef xr_os_io_path_utime_now
#undef xr_os_io_path_symlink
#undef xr_os_io_path_readlink
#undef xr_os_io_path_realpath
#undef xr_os_io_temp_directory
#undef xr_os_io_temp_file
#undef xr_os_io_temp_finish
#undef xr_os_io_directory_open
#undef xr_os_io_directory_next
#undef xr_os_io_directory_close
#undef xr_os_io_file_metadata
#undef xr_os_io_file_metadata_close
#undef xr_os_cpu_count
#undef xr_thread_pin_to_cpu
#undef xr_os_io_chmod
#undef xr_os_io_chdir
enum { HOST_NORMAL, HOST_OOM_WIDE, HOST_CANCEL_BEFORE, HOST_CANCEL_CONVERSION, HOST_CANCEL_AFTER, HOST_BAD_UTF, HOST_CANCEL_COUNT };
static unsigned host_mode,host_calls,count_calls,pin_calls,last_cpu;
static void (*host_count_probe)(void);
static uint64_t host_cstring_requested;
static XrXirInstance *host_active;
static XrOsIoPolicy host_policy;
enum { SYSTEM_NORMAL, SYSTEM_OOM_AFTER, SYSTEM_CANCEL_AFTER, SYSTEM_IO_AFTER,
 SYSTEM_CLOSE_ONCE, SYSTEM_CLOSE_PERSISTENT, SYSTEM_CANCEL_BEFORE, SYSTEM_SOME_ZERO };
static unsigned system_mode,system_calls[8],system_close_calls;
static uint64_t system_provider_work;
static unsigned system_current_calls,system_current_failures,system_current_failure_at;
typedef struct HostSystemCurrent {void *context;bool(*current)(void *);} HostSystemCurrent;
static bool host_system_current_proxy(void *context){
 HostSystemCurrent *borrow=context;++system_current_calls;bool accepted=borrow->current(borrow->context);
 if(!accepted){++system_current_failures;system_current_failure_at=system_current_calls;}
 return accepted;
}
#if defined(XR_OS_WINDOWS)
static HANDLE system_retained_handle;
static bool system_close_reentry;
#endif
static XrOsIoStatus host_system_after(XrOsIoStatus status){
 if(status!=XR_OS_IO_OK)return status;
 if(system_mode==SYSTEM_OOM_AFTER)runtime_fail_at=runtime_attempts;
 if(system_mode==SYSTEM_CANCEL_AFTER)CHECK(host_active&&xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 return system_mode==SYSTEM_IO_AFTER?XR_OS_IO_IO:status;
}
static XrOsIoStatus host_system_uid(XrOsOptionalI64 *o){
 ++system_calls[0];XrOsIoStatus status=xr_os_io_uid(o);
 /* Adapter injection proves Some(0), not a Windows numeric identity. */
 if(status==XR_OS_IO_OK&&system_mode==SYSTEM_SOME_ZERO)*o=(XrOsOptionalI64){true,0};
 return host_system_after(status);
}
static XrOsIoStatus host_system_gid(XrOsOptionalI64 *o){++system_calls[1];return host_system_after(xr_os_io_gid(o));}
static XrOsIoStatus host_system_ppid(XrOsSystemOwner *o,void *c,bool(*current)(void *),int64_t *out){
 ++system_calls[2];CHECK(host_active);system_provider_work=xr_xir_domain_budget_stats(host_active->domain).work;
 system_current_calls=system_current_failures=system_current_failure_at=0;CHECK(current);
 HostSystemCurrent borrow={c,current};XrOsIoStatus status=xr_os_io_ppid(o,&borrow,host_system_current_proxy,out);
#if defined(XR_OS_WINDOWS)
 if(status==XR_OS_IO_OK&&(system_mode==SYSTEM_CLOSE_ONCE||system_mode==SYSTEM_CLOSE_PERSISTENT)){
  CHECK(!o->handle);HANDLE handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,GetCurrentProcessId());CHECK(handle);
  o->handle=(intptr_t)handle;system_retained_handle=handle;return XR_OS_IO_IO;
 }
#endif
 return host_system_after(status);
}
static XrOsIoStatus host_system_kill(XrOsSystemOwner *o,int64_t pid,int64_t signal,void *c,bool(*current)(void *),bool *out){
 ++system_calls[3];if(system_mode==SYSTEM_CANCEL_BEFORE)CHECK(host_active&&xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 return host_system_after(xr_os_io_kill(o,pid,signal,c,current,out));
}
static XrOsIoStatus host_system_total(int64_t *o){++system_calls[4];return host_system_after(xr_os_io_total_memory(o));}
static XrOsIoStatus host_system_free(XrOsSystemOwner *owner,int64_t *o){++system_calls[5];return host_system_after(xr_os_io_free_memory(owner,o));}
static XrOsIoStatus host_system_uptime(double *o){++system_calls[6];return host_system_after(xr_os_io_uptime(o));}
#if !defined(XR_OS_WINDOWS)
static XrOsIoStatus host_system_loadavg(XrOsLoadAverages *o){++system_calls[7];return host_system_after(xr_os_io_loadavg(o));}
#endif
static XrOsIoStatus host_system_close(XrOsSystemOwner *o){
 ++system_close_calls;
#if defined(XR_OS_WINDOWS)
 if(system_close_reentry){system_close_reentry=false;CHECK(host_active&&xr_xir_instance_free(host_active)==XR_XIR_CALL_BUSY);}
 if(system_mode==SYSTEM_CLOSE_PERSISTENT)return XR_OS_IO_IO;
#endif
 return xr_os_io_system_close(o);
}
static XrOsIoStatus host_allocate_cancel(void *,size_t,void **);
enum { STAT_NORMAL,STAT_ZERO_IDS,STAT_OOM,STAT_CANCEL,STAT_CANCEL_CONVERT,STAT_CANCEL_AFTER,STAT_BAD_UTF,STAT_RETAIN_CLOSE };
static unsigned stat_mode,stat_calls,stat_close_calls,stat_close_failures,stat_vector_flags;
static bool stat_close_reentry;
static XrOsIoStatus host_metadata(const XrOsIoPolicy *policy,const char *path,size_t length,
 bool(*current)(void *),XrOsFileMetadataOwner *owner,XrOsFileMetadata *output){
 ++stat_calls;CHECK(host_active&&policy&&current);XrOsIoPolicy proxy=*policy;
 if(stat_mode==STAT_OOM)runtime_fail_at=runtime_attempts;
 if(stat_mode==STAT_CANCEL)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 if(stat_mode==STAT_CANCEL_CONVERT){host_policy=*policy;proxy.alloc=host_allocate_cancel;}
 if(stat_mode==STAT_BAD_UTF){path="\xFF";length=1;} /* OS-policy injection, never a malformed Source String. */
 XrOsIoStatus status=xr_os_io_file_metadata(&proxy,path,length,current,owner,output);runtime_fail_at=SIZE_MAX;
 /* A synthetic adapter vector tests field geometry, not Windows identity or sampled timestamps. */
 if(status==XR_OS_IO_OK&&output->present&&stat_mode==STAT_ZERO_IDS)
  *output=(XrOsFileMetadata){true,17,23,101,202,303,{true,0},{true,42},false,(stat_vector_flags&2)!=0,(stat_vector_flags&4)!=0};
 if(stat_mode==STAT_CANCEL_AFTER)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
#if defined(XR_OS_WINDOWS)
 if(status==XR_OS_IO_OK&&stat_mode==STAT_RETAIN_CLOSE){
  HANDLE held=CreateFileW(L".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
  CHECK(held!=INVALID_HANDLE_VALUE&&!owner->handle);owner->handle=(intptr_t)held;status=XR_OS_IO_IO;
 }
#endif
 return status;
}
static XrOsIoStatus host_metadata_close(XrOsFileMetadataOwner *owner){
 ++stat_close_calls;
 if(stat_close_reentry){stat_close_reentry=false;CHECK(host_active&&xr_xir_instance_free(host_active)==XR_XIR_CALL_BUSY);}
 if(stat_close_failures){--stat_close_failures;return XR_OS_IO_IO;}
 return xr_os_io_file_metadata_close(owner);
}
enum { IO_BOOL_NORMAL,IO_BOOL_OOM,IO_BOOL_CANCEL_ALLOC,IO_BOOL_CANCEL_AFTER,IO_BOOL_RETAIN_CLOSE };
static unsigned io_bool_mode,io_bool_calls,io_bool_allocations,io_bool_fault_at,io_bool_currents,io_bool_failed_at;
static uint64_t io_bool_entry_work;
static XrOsIoPolicy io_bool_policy;
static bool (*io_bool_current)(void *);
static bool host_io_current(void *context){
 ++io_bool_currents;bool okay=io_bool_current(context);if(!okay)io_bool_failed_at=io_bool_currents;return okay;
}
static XrOsIoStatus host_io_allocate(void *context,size_t bytes,void **output){
 unsigned ordinal=io_bool_allocations++;
 if(ordinal==io_bool_fault_at&&io_bool_mode==IO_BOOL_OOM)runtime_fail_at=runtime_attempts;
 XrOsIoStatus status=io_bool_policy.alloc(context,bytes,output);runtime_fail_at=SIZE_MAX;
 if(ordinal==io_bool_fault_at&&io_bool_mode==IO_BOOL_CANCEL_ALLOC&&status==XR_OS_IO_OK)
  CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 return status;
}
static XrOsIoStatus host_io_operation(const XrOsIoPolicy *policy,XrOsPathSpan first,XrOsPathSpan second,
 bool(*current)(void *),XrOsFileMetadataOwner *owner,bool *output,unsigned op){
 ++io_bool_calls;CHECK(host_active&&policy&&current);io_bool_policy=*policy;XrOsIoPolicy proxy=*policy;proxy.alloc=host_io_allocate;
 io_bool_allocations=io_bool_currents=io_bool_failed_at=0;io_bool_current=current;
 io_bool_entry_work=xr_xir_domain_budget_stats(host_active->domain).work;XrOsIoStatus status=XR_OS_IO_BAD_ARGUMENT;
 switch(op){
 case 0:status=xr_os_io_path_mkdir(&proxy,first,host_io_current,output);break;
 case 1:status=xr_os_io_path_rmdir(&proxy,first,host_io_current,output);break;
 case 2:status=xr_os_io_path_remove(&proxy,first,host_io_current,owner,output);break;
 case 3:status=xr_os_io_path_rename(&proxy,first,second,host_io_current,output);break;
 case 4:status=xr_os_io_path_utime_now(&proxy,first,host_io_current,owner,output);break;
 default:CHECK(false);break;
 }
 io_bool_current=NULL;
 if(io_bool_mode==IO_BOOL_CANCEL_AFTER)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
#if defined(XR_OS_WINDOWS)
 if(status==XR_OS_IO_OK&&io_bool_mode==IO_BOOL_RETAIN_CLOSE){
  CHECK(owner&&!owner->handle);HANDLE held=CreateFileW(L".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
  CHECK(held!=INVALID_HANDLE_VALUE);owner->handle=(intptr_t)held;status=XR_OS_IO_IO;
 }
#endif
 return status;
}
static XrOsIoStatus host_io_mkdir(const XrOsIoPolicy *p,XrOsPathSpan a,bool(*c)(void *),bool *o){return host_io_operation(p,a,(XrOsPathSpan){0},c,NULL,o,0);}
static XrOsIoStatus host_io_rmdir(const XrOsIoPolicy *p,XrOsPathSpan a,bool(*c)(void *),bool *o){return host_io_operation(p,a,(XrOsPathSpan){0},c,NULL,o,1);}
static XrOsIoStatus host_io_remove(const XrOsIoPolicy *p,XrOsPathSpan a,bool(*c)(void *),XrOsFileMetadataOwner *h,bool *o){return host_io_operation(p,a,(XrOsPathSpan){0},c,h,o,2);}
static XrOsIoStatus host_io_rename(const XrOsIoPolicy *p,XrOsPathSpan a,XrOsPathSpan b,bool(*c)(void *),bool *o){return host_io_operation(p,a,b,c,NULL,o,3);}
static XrOsIoStatus host_io_utime(const XrOsIoPolicy *p,XrOsPathSpan a,bool(*c)(void *),XrOsFileMetadataOwner *h,bool *o){return host_io_operation(p,a,(XrOsPathSpan){0},c,h,o,4);}
enum { LINKS_NORMAL,LINKS_OOM,LINKS_CANCEL_BEFORE,LINKS_CANCEL_AFTER,LINKS_RESULT_OOM,LINKS_RETAIN_CLOSE };
static unsigned links_mode,links_calls[3];
static XrOsIoStatus host_links_symlink(const XrOsIoPolicy *policy,XrOsPathSpan source,XrOsPathSpan name,
 bool(*current)(void *),bool *output){
 ++links_calls[0];CHECK(host_active&&current);
 return xr_os_io_path_symlink(policy,source,name,current,output);
}
static XrOsIoStatus host_links_query(const XrOsIoPolicy *policy,XrOsPathSpan path,
 bool(*current)(void *),XrOsFileMetadataOwner *owner,XrOsFileLinkText *output,bool following){
 ++links_calls[following?2:1];CHECK(host_active&&current);
 if(links_mode==LINKS_OOM)runtime_fail_at=runtime_attempts;
 if(links_mode==LINKS_CANCEL_BEFORE)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 XrOsIoStatus status=following?xr_os_io_path_realpath(policy,path,current,owner,output):
  xr_os_io_path_readlink(policy,path,current,owner,output);
 runtime_fail_at=SIZE_MAX;
 if(links_mode==LINKS_CANCEL_AFTER)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 if(status==XR_OS_IO_OK&&links_mode==LINKS_RESULT_OOM)runtime_fail_at=runtime_attempts;
#if defined(XR_OS_WINDOWS)
 /* Adapter injection retains a real handle; it is not a Windows close fault. */
 if(status==XR_OS_IO_OK&&links_mode==LINKS_RETAIN_CLOSE){
  CHECK(!owner->handle);HANDLE held=CreateFileW(L".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
   NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(held!=INVALID_HANDLE_VALUE);
  owner->handle=(intptr_t)held;status=XR_OS_IO_IO;
 }
#endif
 return status;
}
static XrOsIoStatus host_links_readlink(const XrOsIoPolicy *p,XrOsPathSpan a,bool(*c)(void *),XrOsFileMetadataOwner *h,XrOsFileLinkText *o){return host_links_query(p,a,c,h,o,false);}
static XrOsIoStatus host_links_realpath(const XrOsIoPolicy *p,XrOsPathSpan a,bool(*c)(void *),XrOsFileMetadataOwner *h,XrOsFileLinkText *o){return host_links_query(p,a,c,h,o,true);}
enum { IO_TEMP_NORMAL,IO_TEMP_PREPARE_OOM,IO_TEMP_PREPARE_CANCEL,IO_TEMP_KEEP_FAIL,IO_TEMP_AFTER_KEEP_OOM };
static unsigned io_temp_mode,io_temp_creates,io_temp_keeps,io_temp_rollbacks,io_temp_rollback_failures;
static char io_temp_candidate[65536];
typedef struct HostTempRequest {const XrOsFileTempRequest *actual;} HostTempRequest;
static bool host_temp_current_proxy(void *context){
 HostTempRequest *borrow=context;return !borrow->actual->current||borrow->actual->current(borrow->actual->context);
}
static XrOsIoStatus host_temp_prepare_proxy(void *context,const char *text,size_t length){
 HostTempRequest *borrow=context;CHECK(length<sizeof(io_temp_candidate));memcpy(io_temp_candidate,text,length);io_temp_candidate[length]=0;
 if(io_temp_mode==IO_TEMP_PREPARE_OOM)runtime_fail_at=runtime_attempts;
 XrOsIoStatus status=borrow->actual->prepare(borrow->actual->context,text,length);runtime_fail_at=SIZE_MAX;
 if(status==XR_OS_IO_OK&&io_temp_mode==IO_TEMP_PREPARE_CANCEL)
  CHECK(host_active&&xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 return status;
}
static XrOsIoStatus host_temp_create(const XrOsFileTempRequest *request,XrOsFileTempOwner *owner,bool *created,bool directory){
 ++io_temp_creates;HostTempRequest borrow={request};XrOsFileTempRequest proxy=*request;
 proxy.context=&borrow;proxy.current=host_temp_current_proxy;proxy.prepare=host_temp_prepare_proxy;
 return directory?xr_os_io_temp_directory(&proxy,owner,created):xr_os_io_temp_file(&proxy,owner,created);
}
static XrOsIoStatus host_temp_directory(const XrOsFileTempRequest *r,XrOsFileTempOwner *o,bool *c){return host_temp_create(r,o,c,true);}
static XrOsIoStatus host_temp_file(const XrOsFileTempRequest *r,XrOsFileTempOwner *o,bool *c){return host_temp_create(r,o,c,false);}
static XrOsIoStatus host_temp_finish(const XrOsIoPolicy *policy,XrOsFileTempOwner *owner,bool keep){
 if(keep){++io_temp_keeps;if(io_temp_mode==IO_TEMP_KEEP_FAIL)return XR_OS_IO_IO;}
 else {++io_temp_rollbacks;if(io_temp_rollback_failures){--io_temp_rollback_failures;return XR_OS_IO_IO;}}
 XrOsIoStatus status=xr_os_io_temp_finish(policy,owner,keep);
 if(keep&&status==XR_OS_IO_OK&&io_temp_mode==IO_TEMP_AFTER_KEEP_OOM)runtime_fail_at=runtime_attempts;
 return status;
}
enum { DIR_NORMAL,DIR_OPEN_CANCEL,DIR_NEXT_OOM,DIR_NEXT_CANCEL,DIR_NEXT_ERROR,DIR_CLOSE_CANCEL };
static unsigned directory_mode,directory_calls[3],directory_close_failures;
static char directory_observed_name[1024];
static void host_directory_cancel(void){
 CHECK(host_active&&xr_xir_call_request_cancel(instance_current(host_active)->call)==XR_XIR_CALL_CANCEL_REQUESTED);
}
static XrOsIoStatus host_directory_open(const XrOsDirectoryRequest *request,XrOsDirectoryOwner *owner,bool *opened){
 ++directory_calls[0];XrOsIoStatus status=xr_os_io_directory_open(request,owner,opened);
 if(status==XR_OS_IO_OK&&*opened&&directory_mode==DIR_OPEN_CANCEL)host_directory_cancel();
 return status;
}
static XrOsIoStatus host_directory_next(XrOsDirectoryOwner *owner,void *context,bool(*current)(void *),XrOsDirectoryText *output){
 ++directory_calls[1];if(directory_mode==DIR_NEXT_ERROR)return XR_OS_IO_IO;
 XrOsIoStatus status=xr_os_io_directory_next(owner,context,current,output);
 if(status==XR_OS_IO_OK){
  CHECK(host_active&&output->bytes&&output->length<sizeof(directory_observed_name));
  memcpy(directory_observed_name,output->bytes,output->length);directory_observed_name[output->length]=0;
  if(directory_mode==DIR_NEXT_OOM)runtime_fail_at=runtime_attempts;
  if(directory_mode==DIR_NEXT_CANCEL)host_directory_cancel();
 }
 return status;
}
static XrOsIoStatus host_directory_close(XrOsDirectoryOwner *owner){
 ++directory_calls[2];
 /* An adapter refusal retains the actual kernel owner; no OS failure is claimed. */
 if(directory_close_failures){--directory_close_failures;return XR_OS_IO_IO;}
 XrOsIoStatus status=xr_os_io_directory_close(owner);
 if(status==XR_OS_IO_OK&&directory_mode==DIR_CLOSE_CANCEL){directory_mode=DIR_NORMAL;host_directory_cancel();}
 return status;
}
static unsigned int host_count(void){
 ++count_calls;
 if(host_count_probe)host_count_probe();
 if(host_mode==HOST_CANCEL_COUNT){host_mode=HOST_NORMAL;CHECK(host_active&&xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);}
 return xr_os_cpu_count();
}
static int host_pin(unsigned int cpu){++pin_calls;last_cpu=cpu;return xr_thread_pin_to_cpu(cpu);}
static XrOsIoStatus host_allocate_cancel(void *context,size_t bytes,void **output){
 XrOsIoStatus status=host_policy.alloc(context,bytes,output);
 if(status==XR_OS_IO_OK)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 return status;
}
static XrOsIoStatus host_mutate(const XrOsIoPolicy *policy,const char *path,int64_t mode,
 bool(*current)(void *),bool *output,bool chmod){
 ++host_calls;CHECK(host_active&&policy&&policy->context&&current);
 host_cstring_requested=xr_xir_domain_budget_stats(host_active->domain).requested_call_bytes;
 if(host_mode==HOST_CANCEL_BEFORE)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 XrOsIoPolicy proxy=*policy;
 if(host_mode==HOST_CANCEL_CONVERSION){host_policy=*policy;proxy.alloc=host_allocate_cancel;}
 if(host_mode==HOST_OOM_WIDE)runtime_fail_at=runtime_attempts;
 if(host_mode==HOST_BAD_UTF)path="\xFF"; /* Policy injection, never an invalid Source String. */
 XrOsIoStatus status=chmod?xr_os_io_chmod(&proxy,path,mode,current,output):xr_os_io_chdir(&proxy,path,current,output);
 runtime_fail_at=SIZE_MAX;
 if(host_mode==HOST_CANCEL_AFTER)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 return status;
}
static XrOsIoStatus host_chmod(const XrOsIoPolicy *p,const char *s,int64_t m,bool(*c)(void *),bool *o){return host_mutate(p,s,m,c,o,true);}
static XrOsIoStatus host_chdir(const XrOsIoPolicy *p,const char *s,bool(*c)(void *),bool *o){return host_mutate(p,s,0,c,o,false);}
static XrXirCallStatus host_poll(XrXirInstance *instance){
 for(unsigned n=0;n<100000;++n){XrXirCallStatus s=xr_xir_instance_poll_bounded(instance,32).outcome.status;if(s!=XR_XIR_CALL_READY)return s;}
 CHECK(false);return XR_XIR_CALL_LIMIT;
}
static XrXirValue host_invoke(XrXirInstance *instance,uint32_t function,const XrXirValue *args,uint32_t count){
 CHECK(xr_xir_instance_start(instance,function,args,count)==XR_XIR_CALL_READY);
 XrXirCallStatus status=host_poll(instance);
 if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"function%u status%u\n",function,status);
 CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstance *host_instance(XrXirProgram *program,uint32_t entry){
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 XrXirValue value=host_invoke(instance,entry,NULL,0);CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);return instance;
}
#endif // HOST_OS_RUNTIME_H
