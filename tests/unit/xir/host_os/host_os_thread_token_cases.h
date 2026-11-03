/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_thread_token_cases.h - Real thread identities and typed lazy initialization
 */
#ifndef HOST_OS_THREAD_TOKEN_CASES_H
#define HOST_OS_THREAD_TOKEN_CASES_H
enum { TOKEN_NORMAL,TOKEN_OOM,TOKEN_IO,TOKEN_CANCEL,TOKEN_CANCEL_INITIALIZER };
enum { HOST_TOKEN_COUNT=15 };
static unsigned token_mode,token_calls;
static uint64_t token_entry_work;

static XrOsIoStatus host_token_current(const XrOsThreadTokenQuery *query,int64_t *output){
 ++token_calls;CHECK(host_active&&query&&query->current);
 token_entry_work=xr_xir_domain_budget_stats(host_active->domain).work;
 if(token_mode==TOKEN_OOM)return XR_OS_IO_OUT_OF_MEMORY;
 if(token_mode==TOKEN_IO)return XR_OS_IO_IO;
 if(token_mode==TOKEN_CANCEL)CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 return xr_os_io_thread_token_current(query,output);
}
static XrOsIoStatus host_token_alive(const XrOsThreadTokenQuery *query,int64_t token,bool *output){
 ++token_calls;CHECK(host_active&&query&&query->current);
 if(token_mode==TOKEN_CANCEL_INITIALIZER&&token==0){
  token_mode=TOKEN_NORMAL;CHECK(xr_xir_call_request_cancel(host_active->call)==XR_XIR_CALL_CANCEL_REQUESTED);
 }
 return xr_os_io_thread_token_alive(query,token,output);
}
static void host_token_limit(XrXirProgram *program,const uint32_t *ids){
 uint64_t limit=0;
 for(unsigned pass=0;pass<3;++pass){
  XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
  if(pass==1)config.work_limit=limit;
  XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
  host_active=instance;token_mode=TOKEN_NORMAL;XrXirValue init=host_invoke(instance,ids[0],NULL,0);xr_xir_value_drop(&init);
  CHECK(xr_xir_instance_start(instance,ids[1],NULL,0)==XR_XIR_CALL_READY);
  XrXirCallStatus status=host_poll(instance);XrXirValue value={0};
  if(pass==1){
   CHECK(status==XR_XIR_CALL_LIMIT&&xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_LIMIT&&!value.type&&!value.payload);
  }else{
   CHECK(status==XR_XIR_CALL_RETURNED&&xr_xir_instance_take_result(instance,&value)==status&&value.type==XR_XIR_I64&&value.payload>0);
   CHECK(token_entry_work<UINT64_MAX);limit=token_entry_work+1;
  }
  xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==(pass==1?XR_XIR_CALL_LIMIT:XR_XIR_CALL_READY));host_active=NULL;
 }
}
static void host_thread_tokens_execute(XrXirProgram *program,const uint32_t *ids){
 size_t live=runtime_live,bytes=runtime_bytes;int64_t reference=0;
 CHECK(xr_os_io_thread_token_current(NULL,&reference)==XR_OS_IO_OK&&reference>0);
 XrXirInstance *instance=host_instance(program,ids[0]);host_active=instance;token_mode=TOKEN_NORMAL;
 CHECK(program->permissions&&!program->permissions->entries[ids[13]].requires_root&&!program->permissions->entries[ids[14]].requires_root);
 CHECK(xr_xir_instance_start(instance,ids[13],NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
 XrXirValue private_argument={XR_XIR_I64,0,reference};
 CHECK(xr_xir_instance_start(instance,ids[14],&private_argument,1)==XR_XIR_CALL_BAD_ARGUMENT);
 for(unsigned pass=0;pass<2;++pass){
  XrXirValue value=host_invoke(instance,ids[1],NULL,0);CHECK(value.type==XR_XIR_I64&&value.payload==reference);xr_xir_value_drop(&value);
 }
 const int64_t samples[]={reference,0,-1,INT64_MAX};
 for(unsigned n=0;n<sizeof(samples)/sizeof(*samples);++n){
  bool expected=true;CHECK(xr_os_io_thread_token_alive(NULL,samples[n],&expected)==XR_OS_IO_OK);
  XrXirValue argument={XR_XIR_I64,0,samples[n]},value=host_invoke(instance,ids[2],&argument,1);
  CHECK(value.type==XR_XIR_BOOL&&value.payload==(expected?1:0));xr_xir_value_drop(&value);
 }
 XrXirValue wrapped=host_invoke(instance,ids[3],NULL,0);CHECK(wrapped.type==XR_XIR_I64&&wrapped.payload==reference);xr_xir_value_drop(&wrapped);
 for(unsigned n=4;n<=12;++n){
  if(n==8)continue;
  XrXirValue value=host_invoke(instance,ids[n],NULL,0);CHECK(value.type==XR_XIR_I64&&value.payload==42);xr_xir_value_drop(&value);
 }
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);host_active=NULL;
 bool alive=false;CHECK(xr_os_io_thread_token_alive(NULL,reference,&alive)==XR_OS_IO_OK&&alive);
 for(unsigned mode=TOKEN_OOM;mode<=TOKEN_CANCEL_INITIALIZER;++mode){
  instance=host_instance(program,ids[0]);host_active=instance;token_mode=mode;
  uint32_t function=mode==TOKEN_CANCEL_INITIALIZER?ids[8]:ids[1];
  CHECK(xr_xir_instance_start(instance,function,NULL,0)==XR_XIR_CALL_READY);
  XrXirCallStatus expected=mode==TOKEN_OOM?XR_XIR_CALL_OOM:mode==TOKEN_IO?XR_XIR_CALL_HOST_ERROR:XR_XIR_CALL_CANCELLED;
  CHECK(host_poll(instance)==expected);XrXirValue value={0};
  CHECK(xr_xir_instance_take_result(instance,&value)==expected&&!value.type&&!value.payload);
  CHECK(xr_xir_instance_free(instance)==expected);host_active=NULL;token_mode=TOKEN_NORMAL;
 }
 host_token_limit(program,ids);
 CHECK(runtime_live==live&&runtime_bytes==bytes);
}
#endif // HOST_OS_THREAD_TOKEN_CASES_H
