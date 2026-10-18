/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_copy_bytes_runtime.h - Independent UTF-8 bytes and runtime faults
 */
#ifndef XIR_STRING_COPY_BYTES_RUNTIME_H
#define XIR_STRING_COPY_BYTES_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_array.h"
static void byte_value(const XrXirValue *value,bool empty) {
 const unsigned char expected[]={65,195,169,0,240,159,152,128,66};
 XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=UINT64_MAX,.scratch_bytes=65536};int64_t count=-1;
 CHECK(xr_xir_array_len(value,&admission,&count)==XR_XIR_VALUE_OK && count==(empty?0:9));
 for(int64_t i=0;i<count;++i){XrXirValue item={0};XrXirFaultDetail fault={0};
  CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.type==XR_XIR_U8 && !item.reserved && item.payload==expected[i]);xr_xir_value_drop(&item);
 }
}
static XrXirValue byte_runtime_take(XrXirProgram *program,uint32_t entry,bool empty) {
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);XrXirCallStatus status;unsigned polls=0;
 do{CHECK(++polls<=4096);status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;}while(status==XR_XIR_CALL_READY);
 XrXirValue value={0};CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
 byte_value(&value,empty);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);return value;
}
static void byte_runtime_attempt(XrXirProgram *program,uint32_t entry,bool empty) {
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
 XrXirInstance *instance=NULL;XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);bool driven=false;
 if(status==XR_XIR_CALL_OOM)CHECK(!instance);XrXirValue value={0};
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
 unsigned polls=0;while(status==XR_XIR_CALL_READY){CHECK(++polls<=4096);status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;driven|=status==XR_XIR_CALL_OOM;}
 CHECK(status==XR_XIR_CALL_RETURNED || (runtime_fail_at!=SIZE_MAX && status==XR_XIR_CALL_OOM));
 if(status==XR_XIR_CALL_RETURNED){CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);byte_value(&value,empty);}
 else if(instance){
  XrXirCallStatus expected=xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED || !driven?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_OOM;
  CHECK(xr_xir_instance_take_result(instance,&value)==expected && !value.type && !value.reserved && !value.payload);
 }
 if(instance)CHECK(xr_xir_instance_free(instance)==(driven?XR_XIR_CALL_OOM:XR_XIR_CALL_READY));
 if(value.type){byte_value(&value,empty);xr_xir_value_drop(&value);}
}
static size_t byte_runtime_failures(XrXirProgram *program,uint32_t entry,bool empty,bool inject) {
 size_t blocks=runtime_live,bytes=runtime_bytes;runtime_attempts=0;byte_runtime_attempt(program,entry,empty);size_t sites=runtime_attempts;
 CHECK(sites && runtime_live==blocks && runtime_bytes==bytes);
 if(inject)for(size_t fault=0;fault<sites;++fault){
  runtime_attempts=0;runtime_fail_at=fault;byte_runtime_attempt(program,entry,empty);CHECK(runtime_attempts>fault && runtime_live==blocks && runtime_bytes==bytes);
  runtime_fail_at=SIZE_MAX;byte_runtime_attempt(program,entry,empty);CHECK(runtime_live==blocks && runtime_bytes==bytes);
 }
 return sites;
}
#endif // XIR_STRING_COPY_BYTES_RUNTIME_H
