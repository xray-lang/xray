/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_regex_unicode_runtime.h - Independent property facts across actual engines
 */
#ifndef XIR_REGEX_UNICODE_RUNTIME_H
#define XIR_REGEX_UNICODE_RUNTIME_H
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_array.h"
#include "base/xunicode.c"
static XrXirValue unicode_call(XrXirInstance *instance,uint32_t entry,const XrXirValue *args,uint32_t count) {
 CHECK(xr_xir_instance_start(instance,entry,args,count)==XR_XIR_CALL_READY);XrXirCallStatus done;unsigned polls=0;
 do{CHECK(++polls<4096);done=xr_xir_instance_poll_bounded(instance,1024).outcome.status;}while(done==XR_XIR_CALL_READY);
 XrXirValue value={0};CHECK(done==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static XrXirInstance *unicode_instance(XrXirProgram *program) {
 XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);return instance;
}
static void unicode_scalar(XrXirInstance *instance,uint32_t entry,int64_t cp,int64_t prop,bool expected) {
 XrXirValue args[]={{XR_XIR_I64,0,cp},{XR_XIR_I64,0,prop}};XrXirValue out=unicode_call(instance,entry,args,2);CHECK(out.type==XR_XIR_BOOL && !out.reserved && out.payload==expected);xr_xir_value_drop(&out);
}
static void unicode_range_oracles(XrXirProgram *program,uint32_t entry) {
 size_t points=0;
 for(int prop=1;prop<XR_UP_COUNT;++prop){XrXirInstance *instance=unicode_instance(program);const XrUnicodeRange *ranges=NULL;int count=0;CHECK(xr_unicode_property_ranges((XrUnicodeProperty)prop,&ranges,&count));
  for(int i=0;i<count;++i){const int64_t cps[]={(int64_t)ranges[i].lo-1,ranges[i].lo,ranges[i].hi,(int64_t)ranges[i].hi+1};
   for(unsigned j=0;j<4;++j){int64_t cp=cps[j];bool valid=cp>=0 && cp<=1114111 && !(cp>=55296 && cp<=57343);unicode_scalar(instance,entry,cp,prop,valid && xr_unicode_is_property((uint32_t)cp,(XrUnicodeProperty)prop));++points;}
  }
  unicode_scalar(instance,entry,-1,prop,false);unicode_scalar(instance,entry,55296,prop,false);unicode_scalar(instance,entry,57343,prop,false);unicode_scalar(instance,entry,1114112,prop,false);unicode_scalar(instance,entry,INT64_MAX,prop,false);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 }
 XrXirInstance *instance=unicode_instance(program);unicode_scalar(instance,entry,65,0,false);unicode_scalar(instance,entry,65,-1,false);unicode_scalar(instance,entry,65,20,false);unicode_scalar(instance,entry,65,INT64_MAX,false);unicode_scalar(instance,entry,20013,10,true);unicode_scalar(instance,entry,65,10,false);unicode_scalar(instance,entry,0,19,true);unicode_scalar(instance,entry,1114111,19,true);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 printf("Projected ordinary Unicode: %zu exact canonical C range boundary comparisons,19 IDs and independent scalar/Han/Any/invalid facts\n",points);
}
static void unicode_name_oracles(XrXirProgram *program,uint32_t entry) {
 XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
 for(const PropertyEntry *row=property_table;row->name;++row){XrXirInstance *instance=unicode_instance(program);size_t length=strlen(row->name);CHECK(length<64);
  for(unsigned style=0;style<3;++style){char bytes[64];memcpy(bytes,row->name,length);
   if(style)for(size_t i=0;i<length;++i){unsigned char c=(unsigned char)bytes[i];if(style==1 && c>='A' && c<='Z')bytes[i]=(char)(c+32);if(style==2 && c>='a' && c<='z')bytes[i]=(char)(c-32);}
   XrXirValue input={0};CHECK(xr_xir_string_new(domain,bytes,length,&input)==XR_XIR_VALUE_OK);XrXirValue out=unicode_call(instance,entry,&input,1);CHECK(out.type==XR_XIR_I64 && !out.reserved && out.payload==row->prop && out.payload==xr_unicode_property_lookup(bytes,(int)length));xr_xir_value_drop(&out);xr_xir_value_drop(&input);
  }
  CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 }
 const char *unknown[]={"","unknown","Han\0extra","Han "," 汉","Greekx"};const size_t lengths[]={0,7,9,4,4,6};
 for(unsigned i=0;i<6;++i){XrXirInstance *instance=unicode_instance(program);XrXirValue input={0};CHECK(xr_xir_string_new(domain,unknown[i],lengths[i],&input)==XR_XIR_VALUE_OK);XrXirValue out=unicode_call(instance,entry,&input,1);CHECK(out.type==XR_XIR_I64 && out.payload==-1);xr_xir_value_drop(&out);xr_xir_value_drop(&input);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 xr_xir_domain_drop(domain);
}
static void unicode_folded_value(const XrXirValue *value) {
 const int64_t expected[]={10,1,19,-1,1,0,1,0};XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=UINT64_MAX,.scratch_bytes=65536};int64_t count=-1;CHECK(xr_xir_array_len(value,&admission,&count)==XR_XIR_VALUE_OK && count==8);
 for(int64_t i=0;i<8;++i){XrXirValue item={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.type==XR_XIR_I64 && item.payload==expected[i]);xr_xir_value_drop(&item);}
}
#endif // XIR_REGEX_UNICODE_RUNTIME_H
