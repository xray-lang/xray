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
static unsigned int host_count(void);
static int host_pin(unsigned int);
static XrOsIoStatus host_chmod(const XrOsIoPolicy *,const char *,int64_t,bool(*)(void *),bool *);
static XrOsIoStatus host_chdir(const XrOsIoPolicy *,const char *,bool(*)(void *),bool *);
#define xr_os_cpu_count host_count
#define xr_thread_pin_to_cpu host_pin
#define xr_os_io_chmod host_chmod
#define xr_os_io_chdir host_chdir
#include "../xir_runtime_allocations.h"
#undef xr_os_cpu_count
#undef xr_thread_pin_to_cpu
#undef xr_os_io_chmod
#undef xr_os_io_chdir
enum { HOST_NORMAL, HOST_OOM_WIDE, HOST_CANCEL_BEFORE, HOST_CANCEL_CONVERSION, HOST_CANCEL_AFTER, HOST_BAD_UTF, HOST_CANCEL_COUNT };
static unsigned host_mode,host_calls,count_calls,pin_calls,last_cpu;
static XrXirInstance *host_active;
static XrOsIoPolicy host_policy;
static unsigned int host_count(void){
 ++count_calls;
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
