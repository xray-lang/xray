/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_unicode.c - Ordinary projected queries against independent facts
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_array.h"
#include "xir_regex_private_source.h"
#include "base/xunicode.c"
static const char unicode_consumer[]=
 "export fn property(name:string)->i64{return rxUnicodePropertyId(name)}\n"
 "export fn has(cp:i64,id:i64)->bool{return rxUnicodeHasProperty(cp,id)}\n"
 "export fn folded()->Array<i64>{return comptime [rxUnicodePropertyId(\"Han\"),rxUnicodePropertyId(\"LETTER\"),rxUnicodePropertyId(\"AnY\"),rxUnicodePropertyId(\"Han\\0\"),rxUnicodeHasProperty(20013,10)?1:0,rxUnicodeHasProperty(55296,19)?1:0,rxUnicodeHasProperty(1114111,19)?1:0,rxUnicodeHasProperty(1114112,19)?1:0]}\n";
static uint32_t unicode_entry(const XrXirModule *module,const char *name) {
 uint32_t found=UINT32_MAX;for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}CHECK(found!=UINT32_MAX);return found;
}
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
int main(void) {
 const char *names[]={"\nfn rxUnicodeRanges(","\nfn rxUnicodePropertyId(","\nfn rxUnicodeHasProperty("};size_t length=0;char *helpers=regex_private_source(names,3,&length);size_t size=length+sizeof(unicode_consumer);char *bytes=xr_malloc(size);CHECK(bytes && snprintf(bytes,size,"%s%s",helpers,unicode_consumer)>0);memset(helpers,0xa5,length);xr_free(helpers);
 XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"actual-regex-private-unicode-projection",NULL};XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};XrXirSourceText text={NULL,bytes,strlen(bytes)};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
 fprintf(stderr,"Actual private Unicode Source%u at%d:%d %s; allocated%llu peak%llu work%llu\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);CHECK(status==XR_XIR_OK);xr_compile_session_free(session);memset(bytes,0xa5,size);xr_free(bytes);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
 CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
 const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t property=unicode_entry(module,"property"),has=unicode_entry(module,"has"),folded=unicode_entry(module,"folded");XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
 unicode_range_oracles(program,has);unicode_name_oracles(program,property);XrXirInstance *instance=unicode_instance(program);XrXirValue value=unicode_call(instance,folded,NULL,0);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
 const int64_t expected[]={10,1,19,-1,1,0,1,0};XrXirValueAdmission admission={.arena=xr_xir_value_arena(&value),.work=UINT64_MAX,.scratch_bytes=65536};int64_t count=-1;CHECK(xr_xir_array_len(&value,&admission,&count)==XR_XIR_VALUE_OK && count==8);
 for(int64_t i=0;i<8;++i){XrXirValue item={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(&value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK && item.type==XR_XIR_I64 && item.payload==expected[i]);xr_xir_value_drop(&item);}xr_xir_value_drop(&value);
 CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);puts("Actual private ordinary Unicode projection: canonical alias/case comparison, independent folded8, source/packet/Program/ledger lifetime and physical0; full Regex qualification not inferred");return 0;
}
