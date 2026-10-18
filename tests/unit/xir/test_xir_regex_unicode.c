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
#include "xir_regex_unicode_runtime.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <ctype.h>
#include "xir/xxir_array.h"
#include "xir_regex_private_source.h"
static const char unicode_consumer[]=
 "export fn property(name:string)->i64{return rxUnicodePropertyId(name)}\n"
 "export fn has(cp:i64,id:i64)->bool{return rxUnicodeHasProperty(cp,id)}\n"
 "export fn folded()->Array<i64>{return comptime [rxUnicodePropertyId(\"Han\"),rxUnicodePropertyId(\"LETTER\"),rxUnicodePropertyId(\"AnY\"),rxUnicodePropertyId(\"Han\\0\"),rxUnicodeHasProperty(20013,10)?1:0,rxUnicodeHasProperty(55296,19)?1:0,rxUnicodeHasProperty(1114111,19)?1:0,rxUnicodeHasProperty(1114112,19)?1:0]}\n";
static uint32_t unicode_entry(const XrXirModule *module,const char *name) {
 uint32_t found=UINT32_MAX;for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))){CHECK(found==UINT32_MAX);found=f;}CHECK(found!=UINT32_MAX);return found;
}
int main(int argc,char **argv) {
 bool emit=argc==4 && !strcmp(argv[1],"--emit-native");CHECK(argc==1 || emit);
 const char *names[]={"\nfn rxUnicodeRangeData(","\nfn rxUnicodeRanges(","\nfn rxUnicodePropertyId(","\nfn rxUnicodeHasProperty("};size_t length=0;char *helpers=regex_private_source(names,4,&length);size_t size=length+sizeof(unicode_consumer);char *bytes=xr_malloc(size);CHECK(bytes && snprintf(bytes,size,"%s%s",helpers,unicode_consumer)>0);memset(helpers,0xa5,length);xr_free(helpers);
 XrXirCompileContext context={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"actual-regex-private-unicode-projection",NULL};XrXirSourceRequest request={.session=session,.authority=&authority,.context=&context,.linkage_kind=XR_XIR_PROGRAM};XrXirSourceText text={NULL,bytes,strlen(bytes)};XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&text,&source,&diagnostic,NULL);XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);
 fprintf(stderr,"Actual private Unicode Source%u at%d:%d %s; allocated%llu peak%llu work%llu\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);CHECK(status==XR_XIR_OK);xr_compile_session_free(session);memset(bytes,0xa5,size);xr_free(bytes);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
 if(emit){FILE *file=fopen(argv[3],"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
 CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
 const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t property=unicode_entry(module,"property"),has=unicode_entry(module,"has"),folded=unicode_entry(module,"folded");if(emit){
  uint32_t methods[]={unicode_entry(module,"rxUnicodePropertyId"),unicode_entry(module,"rxUnicodeHasProperty")};XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"fixture_unicode",8388608,&output)==XR_XIR_OK && !strstr(output.text,"xr_unicode_"));
  for(size_t i=0;i<output.length;++i)if(output.text[i]=='('){size_t next=i+1;while(next<output.length && isspace((unsigned char)output.text[next]))++next;CHECK(next==output.length || output.text[next]!='{');}
  FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length && fprintf(file,"\nconst uint32_t fixture_unicode_selection[5]={%uu,%uu,%uu,%uu,%uu};\n",property,has,folded,methods[0],methods[1])>0 && !fclose(file));xr_xir_compile_c_source_free(&output);xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);return 0;
 }
 XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
 unicode_range_oracles(program,has);unicode_name_oracles(program,property);XrXirInstance *instance=unicode_instance(program);XrXirValue value=unicode_call(instance,folded,NULL,0);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
 unicode_folded_value(&value);xr_xir_value_drop(&value);
 CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);puts("Actual private ordinary Unicode projection: canonical alias/case comparison, independent folded8, source/packet/Program/ledger lifetime and physical0; full Regex qualification not inferred");return 0;
}
