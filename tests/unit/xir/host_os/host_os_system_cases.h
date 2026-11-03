/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_system_cases.h - Real system facts and retained close failure boundaries
 *
 * KEY CONCEPT:
 *   Provider injections exercise actual Domain ownership without faking host facts.
 */
#ifndef HOST_OS_SYSTEM_CASES_H
#define HOST_OS_SYSTEM_CASES_H
#if defined(XR_OS_WINDOWS)
#include <tlhelp32.h>
#endif
static int64_t host_parent_reference(void){
#if defined(XR_OS_WINDOWS)
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);CHECK(snapshot!=INVALID_HANDLE_VALUE);
 PROCESSENTRY32W row={0};row.dwSize=(DWORD)sizeof(row);bool found=false;int64_t parent=0;
 CHECK(Process32FirstW(snapshot,&row));
 do{if(row.th32ProcessID==GetCurrentProcessId()){parent=row.th32ParentProcessID;found=true;break;}}while(Process32NextW(snapshot,&row));
 CHECK(CloseHandle(snapshot)&&found);return parent;
#else
 return (int64_t)getppid();
#endif
}
static double host_double(XrXirValue value){double result=0;CHECK(value.type==XR_XIR_F64);memcpy(&result,&value.payload,sizeof(result));return result;}
static void host_system_limit(XrXirProgram *program,const uint32_t ids[9]){
 size_t live=runtime_live,bytes=runtime_bytes;uint64_t limit=0;
 for(unsigned pass=0;pass<3;++pass){
  XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
  if(pass==1)config.work_limit=limit;
  XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
  XrXirValue value=host_invoke(instance,ids[0],NULL,0);CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);
  host_active=instance;system_mode=SYSTEM_NORMAL;unsigned before=system_calls[2];
  CHECK(xr_xir_instance_start(instance,ids[3],NULL,0)==XR_XIR_CALL_READY);XrXirCallStatus status=host_poll(instance);
  CHECK(system_calls[2]==before+1&&!instance->system_owners);
  if(pass==1){
   CHECK(status==XR_XIR_CALL_LIMIT&&xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_LIMIT&&!value.type&&!value.reserved&&!value.payload);
  }else{
   CHECK(status==XR_XIR_CALL_RETURNED&&xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED&&value.type==XR_XIR_I64&&value.payload==host_parent_reference());
   CHECK(system_provider_work&&system_provider_work<UINT64_MAX);limit=system_provider_work+1;xr_xir_value_drop(&value);
  }
  fprintf(stderr,"system finite work pass%u poll%u provider%u\n",pass,status,system_calls[2]-before);
  CHECK(xr_xir_instance_free(instance)==(pass==1?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));host_active=NULL;
  CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
}
static void host_system_failures(XrXirProgram *program,const uint32_t ids[9]){
 const unsigned modes[]={SYSTEM_OOM_AFTER,SYSTEM_CANCEL_AFTER,SYSTEM_IO_AFTER};
 const XrXirCallStatus expected[]={XR_XIR_CALL_OOM,XR_XIR_CALL_CANCELLED,XR_XIR_CALL_HOST_ERROR};
 for(unsigned n=0;n<sizeof(modes)/sizeof(*modes);++n){
  size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;
  unsigned before=system_calls[0];system_mode=modes[n];
  CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
  XrXirCallStatus status=host_poll(instance);runtime_fail_at=SIZE_MAX;system_mode=SYSTEM_NORMAL;
  CHECK(status==expected[n]&&system_calls[0]==before+1);XrXirValue value={0};
  XrXirCallStatus taken=xr_xir_instance_take_result(instance,&value);
  fprintf(stderr,"system mode%u poll%u take%u\n",modes[n],status,taken);
  /* A cancelled ordinary Call has no transferable RETURNED/THROWN value. */
  CHECK(taken==(n==1?XR_XIR_CALL_BAD_STATE:expected[n])&&!value.type&&!value.reserved&&!value.payload);
  CHECK(xr_xir_instance_free(instance)==(n==1?XR_XIR_CALL_READY:expected[n]));host_active=NULL;
  CHECK(runtime_live==live&&runtime_bytes==bytes);
 }
#if defined(XR_OS_WINDOWS)
 for(unsigned persistent=0;persistent<2;++persistent){
  size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;
  system_mode=persistent?SYSTEM_CLOSE_PERSISTENT:SYSTEM_CLOSE_ONCE;unsigned called=system_calls[2],closed=system_close_calls;
  CHECK(xr_xir_instance_start(instance,ids[3],NULL,0)==XR_XIR_CALL_READY&&host_poll(instance)==XR_XIR_CALL_HOST_ERROR);
  CHECK(system_calls[2]==called+1&&system_close_calls==closed&&instance->system_owners&&
   instance->system_owners->owner.handle==(intptr_t)system_retained_handle);
  XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_HOST_ERROR&&!value.type&&!value.payload);
  system_close_reentry=true;
  XrXirCallStatus freed=xr_xir_instance_free(instance);CHECK(!system_close_reentry&&system_close_calls==closed+1);
  if(persistent){
   DWORD flags=0;CHECK(freed==XR_XIR_CALL_CLOSE_FAILED&&instance->close_drained&&GetHandleInformation(system_retained_handle,&flags));
   CHECK(instance->system_owners&&system_calls[2]==called+1);system_mode=SYSTEM_NORMAL;
   CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_HOST_ERROR&&system_close_calls==closed+2);
  }else CHECK(freed==XR_XIR_CALL_HOST_ERROR);
  DWORD flags=0;CHECK(!GetHandleInformation(system_retained_handle,&flags));system_retained_handle=NULL;
  CHECK(system_calls[2]==called+1&&runtime_live==live&&runtime_bytes==bytes);host_active=NULL;system_mode=SYSTEM_NORMAL;
 }
#endif
}
static void host_system_execute(XrXirProgram *program,const uint32_t ids[9]){
 size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;system_mode=SYSTEM_NORMAL;
 XrXirValue kept[2]={{0}};
 for(unsigned kind=0;kind<2;++kind){
  unsigned before=system_calls[kind];kept[kind]=host_invoke(instance,ids[kind+1],NULL,0);
  bool present=true;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(kept+kind,&present,&payload)&&system_calls[kind]==before+1);
#if defined(XR_OS_WINDOWS)
  CHECK(!present&&!payload);
#else
  CHECK(present&&payload&&payload->type==XR_XIR_I64&&payload->payload==(int64_t)(kind?getgid():getuid()));
#endif
 }
 unsigned before=system_calls[2];XrXirValue value=host_invoke(instance,ids[3],NULL,0);
 CHECK(value.type==XR_XIR_I64&&value.payload==host_parent_reference()&&system_calls[2]==before+1);xr_xir_value_drop(&value);
 XrXirValue args[]={{XR_XIR_I64,0,xr_proc_self_pid()},{XR_XIR_I64,0,0}};
 XrOsSystemOwner reference={0};bool signalled=false;
 CHECK(xr_os_io_kill(&reference,args[0].payload,0,NULL,NULL,&signalled)==XR_OS_IO_OK&&!reference.handle);
 before=system_calls[3];value=host_invoke(instance,ids[4],args,2);
 CHECK(value.type==XR_XIR_BOOL&&value.payload==(signalled?1:0)&&system_calls[3]==before+1);xr_xir_value_drop(&value);
 int64_t total=0;CHECK(xr_os_io_total_memory(&total)==XR_OS_IO_OK&&total>0);
 before=system_calls[4];value=host_invoke(instance,ids[5],NULL,0);
 CHECK(value.type==XR_XIR_I64&&value.payload==total&&system_calls[4]==before+1);xr_xir_value_drop(&value);
 before=system_calls[5];value=host_invoke(instance,ids[6],NULL,0);
 CHECK(value.type==XR_XIR_I64&&value.payload>=0&&value.payload<=total&&system_calls[5]==before+1);xr_xir_value_drop(&value);
 double low=0,high=0;CHECK(xr_os_io_uptime(&low)==XR_OS_IO_OK);before=system_calls[6];value=host_invoke(instance,ids[7],NULL,0);
 CHECK(xr_os_io_uptime(&high)==XR_OS_IO_OK&&host_double(value)>=low&&host_double(value)<=high&&system_calls[6]==before+1);xr_xir_value_drop(&value);
 CHECK(!instance->system_owners&&xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 /* These actual Optional values retain their metadata after the producer dies. */
 for(unsigned kind=0;kind<2;++kind){bool present=false;const XrXirValue *payload=NULL;CHECK(xr_xir_nullable_view(kept+kind,&present,&payload));xr_xir_value_drop(kept+kind);}
 CHECK(runtime_live==live&&runtime_bytes==bytes);
 instance=host_instance(program,ids[0]);host_active=instance;system_mode=SYSTEM_SOME_ZERO;
 XrXirValue zero=host_invoke(instance,ids[1],NULL,0);bool present=false;const XrXirValue *payload=NULL;
 CHECK(xr_xir_nullable_view(&zero,&present,&payload)&&present&&payload&&payload->type==XR_XIR_I64&&!payload->payload);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;system_mode=SYSTEM_NORMAL;
 CHECK(xr_xir_nullable_view(&zero,&present,&payload)&&present&&payload&&!payload->payload);xr_xir_value_drop(&zero);
 CHECK(runtime_live==live&&runtime_bytes==bytes);
 host_system_limit(program,ids);
 host_system_failures(program,ids);
 instance=host_instance(program,ids[0]);host_active=instance;system_mode=SYSTEM_CANCEL_BEFORE;
 CHECK(xr_xir_instance_start(instance,ids[4],args,2)==XR_XIR_CALL_READY&&host_poll(instance)==XR_XIR_CALL_CANCELLED);
 XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE&&!result.type&&!result.payload);
 CHECK(!instance->system_owners&&xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;system_mode=SYSTEM_NORMAL;
 CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif
