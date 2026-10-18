/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_utf8.c - Actual private regex UTF-8 helpers with independent oracles
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
static const char utf8_cases[]=
 "fn matrix()->Array<i64>{return ["
 "rxDecodeUtf8([0],0),rxDecodeUtf8([127],0),rxDecodeUtf8([194,128],0),rxDecodeUtf8([223,191],0),"
 "rxDecodeUtf8([224,160,128],0),rxDecodeUtf8([237,159,191],0),rxDecodeUtf8([238,128,128],0),rxDecodeUtf8([239,191,191],0),"
 "rxDecodeUtf8([240,144,128,128],0),rxDecodeUtf8([244,143,191,191],0),rxDecodeUtf8([240,159,152,128],0),"
 "rxDecodeUtf8([],0),rxDecodeUtf8([65],-1),rxDecodeUtf8([65],1),"
 "rxDecodeUtf8([128],0),rxDecodeUtf8([192,128],0),rxDecodeUtf8([193,191],0),rxDecodeUtf8([194],0),rxDecodeUtf8([194,65],0),"
 "rxDecodeUtf8([224,159,191],0),rxDecodeUtf8([225,128],0),rxDecodeUtf8([225,128,65],0),"
 "rxDecodeUtf8([237,160,128],0),rxDecodeUtf8([237,191,191],0),"
 "rxDecodeUtf8([240,143,191,191],0),rxDecodeUtf8([241,128,128],0),rxDecodeUtf8([241,128,128,65],0),"
 "rxDecodeUtf8([244,144,128,128],0),rxDecodeUtf8([245,128,128,128],0),rxDecodeUtf8([255],0)]}\n"
 "export fn runtime()->Array<i64>{return matrix()}\n"
 "export fn folded()->Array<i64>{return comptime matrix()}\n"
 "export fn text()->string{return rxOwnedUtf8([65,195,169,0,240,159,152,128,66])}\n"
 "export fn textFolded()->string{return comptime rxOwnedUtf8([65,195,169,0,240,159,152,128,66])}\n"
 "export fn empty()->string{return comptime rxOwnedUtf8([])}\n";
static char *utf8_actual_helpers(size_t *length) {
 const char *names[]={"\nfn rxDecodeUtf8(","\nfn rxOwnedUtf8("};return regex_private_source(names,2,length);
}
static XrXirStatus utf8_source(const XrXirCompileContext *context,const char *source,XrXirSourceResult *out,XrXirSourceDiagnostic *diagnostic) {
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"actual-regex-private-utf8-helpers",NULL};
 XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};XrXirSourceText input={NULL,source,strlen(source)};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,out,diagnostic,NULL);xr_compile_session_free(session);return status;
}
static void utf8_context(XrXirCompileContext *context) {
 const XrCompileResourceLimits caps={67108864,8388608,128000000};CHECK(xr_compile_resources_new(&caps,&context->resources)==XR_COMPILE_RESOURCE_OK);context->limits=xr_xir_compile_default_limits();
}
static uint32_t utf8_entry(const XrXirModule *module,const char *name) {
 uint32_t found=UINT32_MAX;for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}CHECK(found!=UINT32_MAX);return found;
}
static void utf8_value(const XrXirValue *value,unsigned which) {
 if(which<2){
  const int64_t codepoints[]={0,127,128,2047,2048,55295,57344,65535,65536,1114111,128512};const int widths[]={1,1,2,2,3,3,3,3,4,4,4};
  XrXirValueAdmission admission={.arena=xr_xir_value_arena(value),.work=UINT64_MAX,.scratch_bytes=65536};int64_t count=-1;CHECK(xr_xir_array_len(value,&admission,&count)==XR_XIR_VALUE_OK && count==30);
  for(int64_t i=0;i<count;++i){XrXirValue item={0};XrXirFaultDetail fault={0};CHECK(xr_xir_array_get(value,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
   CHECK(item.type==XR_XIR_I64 && !item.reserved && item.payload==(i<11?(codepoints[i]<<8)|widths[i]:-1));xr_xir_value_drop(&item);}
 }else{const unsigned char expected[]={65,195,169,0,240,159,152,128,66};const char *bytes=NULL;size_t size=0;
  CHECK(value->type==XR_XIR_STRING && xr_xir_string_view(value,&bytes,&size) && size==(which==4?0:sizeof(expected)) && (!size || !memcmp(bytes,expected,size)));
 }
}
static void utf8_normal(const char *helpers) {
 size_t size=strlen(helpers)+sizeof(utf8_cases);char *text=xr_malloc(size);CHECK(text);CHECK(snprintf(text,size,"%s%s",helpers,utf8_cases)>0);
 XrXirCompileContext context={0};utf8_context(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=utf8_source(&context,text,&source,&diagnostic);if(status!=XR_XIR_OK)fprintf(stderr,"Private actual UTF8 Source%u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);CHECK(status==XR_XIR_OK);
 memset(text,0xa5,size);xr_free(text);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
 XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
 CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
 const XrXirModule *module=xr_xir_compile_artifact_module(lowered);const char *names[]={"runtime","folded","text","textFolded","empty"};uint32_t entries[5];for(unsigned i=0;i<5;++i)entries[i]=utf8_entry(module,names[i]);
 XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);XrXirValue values[5]={{0}};
 for(unsigned i=0;i<5;++i){XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);
  XrXirCallStatus done;unsigned polls=0;do{CHECK(++polls<4096);done=xr_xir_instance_poll_bounded(instance,1024).outcome.status;}while(done==XR_XIR_CALL_READY);
  CHECK(done==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&values[i])==XR_XIR_CALL_RETURNED);utf8_value(&values[i],i);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 }
 xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);for(unsigned i=0;i<5;++i){utf8_value(&values[i],i);xr_xir_value_drop(&values[i]);}
}
static void utf8_invalid_owned(const char *helpers) {
 const char *invalid[]={"[128]","[192,128]","[193,191]","[194]","[194,65]","[224,159,191]","[225,128]","[225,128,65]","[237,160,128]","[237,191,191]","[240,143,191,191]","[241,128,128]","[241,128,128,65]","[244,144,128,128]","[245,128,128,128]","[255]"};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i){size_t size=strlen(helpers)+256;char *text=xr_malloc(size);CHECK(text);int n=snprintf(text,size,"%s\nexport fn rejected()->string{return comptime rxOwnedUtf8(%s)}\n",helpers,invalid[i]);CHECK(n>0 && (size_t)n<size);
  XrXirCompileContext context={0};utf8_context(&context);XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=utf8_source(&context,text,&source,&diagnostic);
  CHECK(status==XR_XIR_BAD_TYPE && !source.checked && !source.snapshot && strstr(diagnostic.message,"comptime assertion failed"));memset(text,0xa5,size);xr_free(text);xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
 }
}
int main(void) {
 size_t length=0;char *helpers=utf8_actual_helpers(&length);CHECK(length==strlen(helpers));utf8_normal(helpers);utf8_invalid_owned(helpers);memset(helpers,0xa5,length);xr_free(helpers);
 CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);puts("Actual private Regex helpers only:30 independent scalar/width cases,16 malformed owned refusals, CTFE/VM, NUL/multibyte text after all producer/Program/ledger callers die; full Regex qualification not inferred");return 0;
}
