/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_regex_precompile_runtime.h - Independent regex program and runtime ownership
 */
#ifndef XIR_REGEX_PRECOMPILE_RUNTIME_H
#define XIR_REGEX_PRECOMPILE_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_array.h"
#include "xir/xxir_types.h"
static const char *patterns[]={"","a","ab*","a*b*","ab*a"};
static const char *texts[]={"","a","b","ab","abb","aaabbb","aba","abba","bb","c","ac"};
static inline bool regex_reference(const char *pattern,const char *text) {
 if(!*pattern)return !*text;
 bool equal=*text && *text==*pattern;
 if(pattern[1]=='*')return regex_reference(pattern+2,text) || (equal && regex_reference(pattern,text+1));
 return equal && regex_reference(pattern+1,text+1);
}
static inline void regex_value(const XrXirValue *value,unsigned which) {
 if(which==2){CHECK(value->type==XR_XIR_I64 && value->payload==42 && !value->reserved);return;}
 XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=UINT64_MAX,.scratch_bytes=65536};int64_t length=-1;
 CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK);
 const int64_t program[]={1,2,97,0,98,1};CHECK(length==(which?6:55));
 for(int64_t i=0;i<length;++i){XrXirValue item={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
  if(which)CHECK(item.type==XR_XIR_I64 && !item.reserved && item.payload==program[i]);
  else CHECK(item.type==XR_XIR_BOOL && !item.reserved && item.payload==regex_reference(patterns[i/11],texts[i%11]));
  xr_xir_value_drop(&item);
 }
}

static inline XrXirValue regex_runtime_take(XrXirProgram *program,uint32_t entry) {
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
 XrXirCallStatus status;unsigned polls=0;
 do{CHECK(++polls<=4096);status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;}while(status==XR_XIR_CALL_READY);
 XrXirValue value={0};CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
 CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return value;
}
static inline void regex_runtime_attempt(XrXirProgram *program,uint32_t entry,unsigned which) {
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);bool driven=false;
 if(status==XR_XIR_CALL_OOM)CHECK(!instance);XrXirValue value={0};
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
 unsigned polls=0;while(status==XR_XIR_CALL_READY){CHECK(++polls<=4096);status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;driven|=status==XR_XIR_CALL_OOM;}
 CHECK(status==XR_XIR_CALL_RETURNED || (runtime_fail_at!=SIZE_MAX && status==XR_XIR_CALL_OOM));
 if(status==XR_XIR_CALL_RETURNED){CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);regex_value(&value,which);}
 else if(instance){CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_BAD_STATE);CHECK(!value.type && !value.reserved && !value.payload);}
 if(instance)CHECK(xr_xir_instance_free(instance)==(driven?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
 if(value.type){regex_value(&value,which);xr_xir_value_drop(&value);}
}
static inline size_t regex_runtime_failures(XrXirProgram *program,uint32_t entry,unsigned which,bool inject) {
 size_t blocks=runtime_live,bytes=runtime_bytes;runtime_attempts=0;regex_runtime_attempt(program,entry,which);size_t sites=runtime_attempts;
 CHECK(sites && runtime_live==blocks && runtime_bytes==bytes);
 if(inject)for(size_t fault=0;fault<sites;++fault){
  runtime_attempts=0;runtime_fail_at=fault;regex_runtime_attempt(program,entry,which);CHECK(runtime_attempts>fault && runtime_live==blocks && runtime_bytes==bytes);
  runtime_fail_at=SIZE_MAX;regex_runtime_attempt(program,entry,which);CHECK(runtime_live==blocks && runtime_bytes==bytes);
 }
 return sites;
}
#endif // XIR_REGEX_PRECOMPILE_RUNTIME_H
