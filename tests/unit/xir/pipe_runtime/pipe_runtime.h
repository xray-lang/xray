/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pipe_runtime.h - Real endpoint bytes, public wakes and independent ownership
 *
 * KEY CONCEPT:
 *   Only the public poll/wait/resume protocol drives suspended operations. Tests
 *   inspect returned values and visible endpoints, never private operation nodes.
 */
#ifndef PIPE_RUNTIME_H
#define PIPE_RUNTIME_H
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(XR_OS_WINDOWS)
#include <io.h>
#include <fcntl.h>
#endif
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Pipe product %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../xir_runtime_allocations.h"
#include "pipe_cases.h"
static void pipe_trace(const char *message){
 const char *enabled=getenv("XR_PIPE_TRACE");if(enabled&&!strcmp(enabled,"1"))fprintf(stderr,"Pipe trace: %s\n",message);
}
static void pipe_trace_status(const char *message,XrXirCallStatus status){
 const char *enabled=getenv("XR_PIPE_TRACE");if(enabled&&!strcmp(enabled,"1"))fprintf(stderr,"Pipe trace: %s status%u\n",message,status);
}
static void pipe_resume(XrXirInstance *instance,XrXirInstanceResult event){
 XrXirWaitRequest wait={0};CHECK(event.outcome.status==XR_XIR_CALL_SUSPENDED&&event.epoch);
 CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&wait)==XR_XIR_CALL_READY&&wait.kind==XR_XIR_WAIT_YIELD);
 CHECK(xr_xir_instance_resume(instance,event.epoch,event.outcome.wake)==XR_XIR_CALL_READY);
}
static XrXirInstanceResult pipe_drive(XrXirInstance *instance){
 for(unsigned n=0;n<1000000;++n){
  XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
  if(event.outcome.status==XR_XIR_CALL_SUSPENDED)pipe_resume(instance,event);
  else if(event.outcome.status!=XR_XIR_CALL_READY)return event;
 }
 CHECK(false);return (XrXirInstanceResult){0};
}
static XrXirValue pipe_invoke(XrXirInstance *instance,uint32_t entry,const XrXirValue *args,uint32_t count){
 CHECK(xr_xir_instance_start(instance,entry,args,count)==XR_XIR_CALL_READY);
 XrXirInstanceResult event=pipe_drive(instance);
 if(event.outcome.status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"Pipe entry%u status%u\n",entry,event.outcome.status);
 CHECK(event.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstance *pipe_instance(XrXirProgram *program,const uint32_t *ids){
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
 CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);XrXirValue entry=pipe_invoke(instance,ids[PIPE_ENTRY],NULL,0);
 CHECK(entry.type==XR_XIR_I64&&!entry.payload);xr_xir_value_drop(&entry);return instance;
}
static XrXirValue pipe_open(XrXirInstance *instance,const uint32_t *ids){
 XrXirValue optional=pipe_invoke(instance,ids[PIPE_OPEN],NULL,0),value={0};bool present=false;const XrXirValue *payload=NULL;
 CHECK(xr_xir_nullable_view(&optional,&present,&payload)&&present&&payload&&xr_xir_value_copy(payload,&value)==XR_XIR_VALUE_OK);xr_xir_value_drop(&optional);return value;
}
static XrXirValueAdmission pipe_access(const XrXirValue *value){
 CHECK(xr_xir_value_valid(value));return (XrXirValueAdmission){.arena=xr_xir_value_arena(value),.domain=object_pointer(value)->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
}
static void pipe_tokens(XrXirInstance *instance,const uint32_t *ids,const XrXirValue *pipe,XrXirValue tokens[2]){
 XrXirValue pair=pipe_invoke(instance,ids[PIPE_ENDS],pipe,1);
 for(unsigned n=0;n<2;++n){xr_xir_value_drop(&tokens[n]);CHECK(xr_xir_tuple_get(&pair,n,&tokens[n])==XR_XIR_VALUE_OK&&tokens[n].type==XR_XIR_I64);}
 xr_xir_value_drop(&pair);
}
static void pipe_bool(XrXirInstance *instance,uint32_t entry,const XrXirValue *arg,bool expected){
 XrXirValue value=pipe_invoke(instance,entry,arg,1);CHECK(value.type==XR_XIR_BOOL&&value.payload==expected);xr_xir_value_drop(&value);
}
static XrXirValue pipe_data(XrXirInstance *instance,XrXirProgram *program,const uint32_t *ids,size_t count){
 CHECK(count<=sizeof(pipe_bytes));XrXirValue elements[sizeof(pipe_bytes)]={0},data={0};
 for(size_t n=0;n<count;++n)elements[n]=(XrXirValue){XR_XIR_U8,0,pipe_bytes[n]};
 XrXirValueAdmission access={.arena=program->arena,.domain=instance->domain,.work=UINT64_MAX,.scratch_bytes=UINT64_MAX};
 CHECK(xr_xir_array_new(program->entries[ids[PIPE_WRITE]].parameters[1],elements,count,&access,&data)==XR_XIR_VALUE_OK);return data;
}
static const XrXirValue *pipe_some(const XrXirValue *optional,bool expected){
 bool present=false;const XrXirValue *value=NULL;CHECK(xr_xir_nullable_view(optional,&present,&value)&&present==expected);
 CHECK(expected?value!=NULL:value==NULL);return value;
}
static size_t pipe_check_bytes(const XrXirValue *array,size_t offset,size_t maximum){
 XrXirValueAdmission access=pipe_access(array);int64_t length=-1;CHECK(xr_xir_array_len(array,&access,&length)==XR_XIR_VALUE_OK&&length>=0&&(uint64_t)length<=maximum);
 for(int64_t n=0;n<length;++n){XrXirValue byte={0};XrXirFaultDetail fault={0};
  CHECK(offset+(size_t)n<sizeof(pipe_bytes)&&xr_xir_array_get(array,n,&access,&byte,&fault)==XR_XIR_VALUE_OK&&byte.type==XR_XIR_U8&&byte.payload==pipe_bytes[offset+(size_t)n]);xr_xir_value_drop(&byte);}
 return (size_t)length;
}
static void pipe_roundtrip(XrXirProgram *program,const uint32_t *ids){
 XrXirInstance *instance=pipe_instance(program,ids);XrXirValue pipe=pipe_open(instance,ids),tokens[2]={{0},{0}},data=pipe_data(instance,program,ids,sizeof(pipe_bytes));
 pipe_tokens(instance,ids,&pipe,tokens);CHECK(tokens[0].payload>0&&tokens[1].payload>0&&tokens[0].payload!=tokens[1].payload);
 XrXirValue args[]={pipe,{XR_XIR_I64,0,0}},zero=pipe_invoke(instance,ids[PIPE_READ],args,2);CHECK(!pipe_check_bytes(pipe_some(&zero,true),0,0));xr_xir_value_drop(&zero);
 XrXirValue empty=pipe_data(instance,program,ids,0);args[1]=empty;XrXirValue wrote=pipe_invoke(instance,ids[PIPE_WRITE],args,2);CHECK(wrote.type==XR_XIR_I64&&!wrote.payload);xr_xir_value_drop(&wrote);xr_xir_value_drop(&empty);
 args[1]=data;wrote=pipe_invoke(instance,ids[PIPE_WRITE],args,2);CHECK(wrote.type==XR_XIR_I64&&wrote.payload==(int64_t)sizeof(pipe_bytes));xr_xir_value_drop(&wrote);CHECK(pipe_check_bytes(&data,0,sizeof(pipe_bytes))==sizeof(pipe_bytes));
 pipe_bool(instance,ids[PIPE_CLOSE_WRITE],&pipe,true);pipe_bool(instance,ids[PIPE_CLOSE_WRITE],&pipe,true);
 size_t consumed=0;XrXirValue retained={0};
 for(;;){args[1]=(XrXirValue){XR_XIR_I64,0,2};XrXirValue part=pipe_invoke(instance,ids[PIPE_READ],args,2);
  size_t count=pipe_check_bytes(pipe_some(&part,true),consumed,2);if(!count){CHECK(consumed==sizeof(pipe_bytes));xr_xir_value_drop(&part);break;}
  if(!retained.type){CHECK(xr_xir_value_copy(pipe_some(&part,true),&retained)==XR_XIR_VALUE_OK);}consumed+=count;xr_xir_value_drop(&part);
 }
 pipe_bool(instance,ids[PIPE_CLOSE_READ],&pipe,true);pipe_bool(instance,ids[PIPE_CLOSE_READ],&pipe,true);pipe_bool(instance,ids[PIPE_CLOSE_ALL],&pipe,true);
 pipe_bool(instance,ids[PIPE_RAW_CLOSE],tokens,false);xr_xir_value_drop(&data);xr_xir_value_drop(&pipe);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);CHECK(pipe_check_bytes(&retained,0,2)>0);xr_xir_value_drop(&retained);
}
static void pipe_refusals(XrXirProgram *program,const uint32_t *ids){
 XrXirInstance *a=pipe_instance(program,ids),*b=pipe_instance(program,ids);XrXirValue p=pipe_open(a,ids),q=pipe_open(b,ids),tokens[2]={{0},{0}},other[2]={{0},{0}};
 pipe_tokens(a,ids,&p,tokens);pipe_tokens(b,ids,&q,other);CHECK(tokens[0].payload!=other[0].payload&&tokens[1].payload!=other[1].payload);
 XrXirValue data=pipe_data(a,program,ids,1),args[]={tokens[1],{XR_XIR_I64,0,0}};
 XrXirValue refused=pipe_invoke(a,ids[PIPE_RAW_READ],args,2);pipe_some(&refused,false);xr_xir_value_drop(&refused);
 args[0]=tokens[0];args[1]=data;refused=pipe_invoke(a,ids[PIPE_RAW_WRITE],args,2);CHECK(refused.type==XR_XIR_I64&&refused.payload==-1);xr_xir_value_drop(&refused);
 args[0]=tokens[0];args[1]=(XrXirValue){XR_XIR_I64,0,0};refused=pipe_invoke(b,ids[PIPE_RAW_READ],args,2);pipe_some(&refused,false);xr_xir_value_drop(&refused);
 pipe_bool(b,ids[PIPE_RAW_CLOSE],tokens,false);XrXirValue fake={XR_XIR_I64,0,0};pipe_bool(b,ids[PIPE_RAW_CLOSE],&fake,false);
 /* A stale native endpoint must not make the real wrapper clear its field. */
 pipe_bool(a,ids[PIPE_RAW_CLOSE],tokens,true);pipe_bool(a,ids[PIPE_CLOSE_READ],&p,false);
 XrXirValue after[2]={{0},{0}};pipe_tokens(a,ids,&p,after);CHECK(after[0].payload==tokens[0].payload);
 pipe_bool(a,ids[PIPE_CLOSE_ALL],&p,false);pipe_tokens(a,ids,&p,after);CHECK(after[0].payload==tokens[0].payload&&after[1].payload==-1);
 pipe_bool(b,ids[PIPE_RAW_CLOSE],other+1,true);pipe_bool(b,ids[PIPE_CLOSE_WRITE],&q,false);pipe_bool(b,ids[PIPE_CLOSE_ALL],&q,false);
 pipe_tokens(b,ids,&q,after);CHECK(after[0].payload==-1&&after[1].payload==other[1].payload);
 xr_xir_value_drop(&data);xr_xir_value_drop(&p);xr_xir_value_drop(&q);
 CHECK(xr_xir_instance_free(a)==XR_XIR_CALL_READY&&xr_xir_instance_free(b)==XR_XIR_CALL_READY);
}
static void pipe_cancel_read(XrXirProgram *program,const uint32_t *ids,bool scoped){
 XrXirInstance *instance=pipe_instance(program,ids);XrXirValue p=pipe_open(instance,ids),args[]={p,{XR_XIR_I64,0,1}};
 CHECK(xr_xir_instance_start(instance,ids[scoped?PIPE_SCOPE_READ:PIPE_READ],args,scoped?1u:2u)==XR_XIR_CALL_READY);bool suspended=false;
 if(scoped){
  /* One public turn observes structured cancellation, not the child's exact
   * IO entry. The separate plain read proves genuinely pending native IO. */
  XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
  pipe_trace_status("scope_cancel bounded observation",event.outcome.status);
  CHECK(event.outcome.status==XR_XIR_CALL_READY||event.outcome.status==XR_XIR_CALL_SUSPENDED);
  if(event.outcome.status==XR_XIR_CALL_SUSPENDED){XrXirWaitRequest wait={0};CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&wait)==XR_XIR_CALL_READY);}
 }else{
  for(unsigned n=0;n<1000000&&!suspended;++n){XrXirInstanceResult event=xr_xir_instance_poll_bounded(instance,64);
   if(event.outcome.status==XR_XIR_CALL_READY)continue;CHECK(event.outcome.status==XR_XIR_CALL_SUSPENDED);
   XrXirWaitRequest wait={0};CHECK(xr_xir_instance_wait_request(instance,event.epoch,event.outcome.wake,&wait)==XR_XIR_CALL_READY);
   if(wait.kind==XR_XIR_WAIT_YIELD){suspended=true;pipe_trace("plain_cancel YIELD observed");}
   else{CHECK(scoped&&wait.kind==XR_XIR_WAIT_TASK);xr_thread_yield();}
  }
  CHECK(suspended);
 }
 CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
 pipe_trace(scoped?"scope_cancel requested":"plain_cancel requested");
 XrXirCallStatus terminal=pipe_drive(instance).outcome.status;pipe_trace_status(scoped?"scope_cancel terminal":"plain_cancel terminal",terminal);
 CHECK(terminal==XR_XIR_CALL_CANCELLED);XrXirValue output={0};CHECK(xr_xir_instance_take_result(instance,&output)==XR_XIR_CALL_BAD_STATE&&!output.type&&!output.payload);
 /* Public terminal cancellation must already have retired both the root read
  * and any scope-owned child read. Close cannot race an orphaned operation. */
 pipe_bool(instance,ids[PIPE_CLOSE_ALL],&p,true);
 pipe_trace(scoped?"scope_cancel endpoints closed":"plain_cancel endpoints closed");
 xr_xir_value_drop(&p);uint64_t started=xr_time_monotonic_ns();CHECK(started);
 /* Cancellation requests termination; only a successful public close retires
  * the owner. Failed closes retry cleanup without executing the guest again. */
 for(;;){
  XrXirCallStatus closed=xr_xir_instance_free(instance);if(closed==XR_XIR_CALL_READY)break;
  CHECK(closed==XR_XIR_CALL_CLOSE_FAILED);uint64_t now=xr_time_monotonic_ns();
  CHECK(now&&now>=started&&now-started<UINT64_C(5000000000));xr_thread_yield();
 }
 pipe_trace(scoped?"scope_cancel instance retired":"plain_cancel instance retired");
}
static void pipe_execute(XrXirProgram *program,const uint32_t *ids){
 size_t live=runtime_live,bytes=runtime_bytes;
 pipe_trace("roundtrip enter");pipe_roundtrip(program,ids);pipe_trace("roundtrip exit");
 pipe_trace("refusals enter");pipe_refusals(program,ids);pipe_trace("refusals exit");
 pipe_trace("plain_cancel enter");pipe_cancel_read(program,ids,false);pipe_trace("plain_cancel exit");
 pipe_trace("scope_cancel enter");pipe_cancel_read(program,ids,true);pipe_trace("scope_cancel exit");
 pipe_trace("handoff enter");XrXirInstance *instance=pipe_instance(program,ids);XrXirValue value=pipe_invoke(instance,ids[PIPE_HANDOFF],NULL,0);CHECK(value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);pipe_trace("handoff exit");
 pipe_trace("defer enter");
 XrXirValue closed=pipe_invoke(instance,ids[PIPE_DEFER_ENDS],NULL,0);
 for(unsigned n=0;n<2;++n){XrXirValue token={0};CHECK(xr_xir_tuple_get(&closed,n,&token)==XR_XIR_VALUE_OK);pipe_bool(instance,ids[PIPE_RAW_CLOSE],&token,false);xr_xir_value_drop(&token);}xr_xir_value_drop(&closed);
 pipe_trace("defer exit");pipe_trace("privateHost enter");
 XrXirValue p=pipe_open(instance,ids),tokens[2]={{0},{0}};pipe_tokens(instance,ids,&p,tokens);
 XrXirValue data=pipe_data(instance,program,ids,1);
 for(unsigned n=0;n<4;++n){uint32_t f=UINT32_MAX;for(uint32_t i=0;i<program->entry_count;++i)if(program->declarations->functions[i].native_callable==97+n)f=i;
  CHECK(f!=UINT32_MAX);XrXirValue args[]={tokens[n==2?1:0],n==2?data:(XrXirValue){XR_XIR_I64,0,0}};
  /* Ordinary host starts reject this private entry before native dispatch. */
  CHECK(xr_xir_instance_start(instance,f,n?args:NULL,n==0?0u:n==3?1u:2u)==XR_XIR_CALL_BAD_ARGUMENT);
 }
 xr_xir_value_drop(&data);xr_xir_value_drop(&p);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 pipe_trace("privateHost exit");
 CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // PIPE_RUNTIME_H
