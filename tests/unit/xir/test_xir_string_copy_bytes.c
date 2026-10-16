/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_copy_bytes.c - Owned UTF-8 bytes through genuine Source
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_string_copy_bytes_runtime.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_array.h"
#include "xir/xxir_emit_c.h"
#include <ctype.h>
static const char byte_baseline[]="export fn run()->Array<u8>{return [65,195,169,0,240,159,152,128,66]}\n";
static const char byte_copy_source[]=
 "fn copy(text:string)->Array<u8>{return text.copyBytes()}\n"
 "export fn run()->Array<u8>{return copy(\"Aé\\0😀B\")}\n"
 "export fn folded()->Array<u8>{return comptime copy(\"Aé\\0😀B\")}\n"
 "export fn empty()->Array<u8>{return comptime \"\".copyBytes()}\n"
 "export fn isolated()->Array<u8>{var text=\"Aé\\0😀B\";var first=text.copyBytes();var second=text.copyBytes();first[0]=90;return second}\n";
static uint32_t byte_function(const XrXirModule *module,const char *name) {
 uint32_t found=UINT32_MAX;size_t length=strlen(name);
 for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)){CHECK(found==UINT32_MAX);found=f;}
 CHECK(found!=UINT32_MAX);return found;
}
static XrXirStatus byte_source(const XrXirCompileContext *context,const char *text,
 const XrXirSourceComptimeLimits *limits,XrXirSourceResult *output,XrXirSourceDiagnostic *report) {
 char bytes[4096],identity[]="string-copy-bytes-consumer";size_t length=strlen(text);CHECK(length<sizeof(bytes));memcpy(bytes,text,length+1);
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,identity,NULL};
 XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM,.comptime_limits=limits};
 XrXirSourceText source={NULL,bytes,length};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"UTF-8 copy Source status%u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
 if(report)*report=diagnostic;
 xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(identity,0xa5,sizeof(identity));
 return status;
}
#include "xir_string_copy_bytes_cases.h"
#include "xir_string_copy_bytes_resources.h"
static void byte_consumer(const char *text,bool copying,const char *emit,const char *packet_path) {
 const XrCompileResourceLimits caps={67108864,8388608,128000000};XrXirCompileContext context={0};
 CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
 XrXirSourceResult source={0};CHECK(byte_source(&context,text,NULL,&source,NULL)==XR_XIR_OK);XrXirCheckedPacket packet={0};
 if(copying)byte_opcode_refusals(source.checked);
 CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
 if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
 XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
 CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
 CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
 const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
 const XrXirModule *module=xr_xir_compile_artifact_module(lowered);const char *names[]={"run","folded","empty","isolated","copy"};uint32_t entries[5];
 for(unsigned i=0;i<(copying?5u:1u);++i)entries[i]=byte_function(module,names[i]);
 if(emit){XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"fixture_bytes",8388608,&output)==XR_XIR_OK);
  for(size_t i=0;i<output.length;++i)if(output.text[i]=='('){size_t next=i+1;while(next<output.length && isspace((unsigned char)output.text[next]))++next;CHECK(next==output.length || output.text[next]!='{');}
  FILE *file=fopen(emit,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length && fprintf(file,"\nconst uint32_t fixture_bytes_selection[5]={%uu,%uu,%uu,%uu,%uu};\n",entries[0],entries[1],entries[2],entries[3],entries[4])>0 && !fclose(file));xr_xir_compile_c_source_free(&output);xr_xir_compile_artifact_free(lowered);xr_compile_resources_release(context.resources);return;
 }
 XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);XrXirValue values[4]={{0}};
 for(unsigned i=0;i<(copying?4u:1u);++i){
  size_t sites=byte_runtime_failures(program,entries[i],i==2,false);printf("Byte-copy VM case%u sites=%zu\n",i,sites);values[i]=byte_runtime_take(program,entries[i],i==2);
 }
 xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
 for(unsigned i=0;i<(copying?4u:1u);++i){byte_value(&values[i],i==2);xr_xir_value_drop(&values[i]);}
 CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
 if(argc==7 && !strcmp(argv[1],"--compiler-fi")){byte_compiler_partition(argv);return 0;}
 if(argc==3 && !strcmp(argv[1],"--emit-source")){FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(byte_copy_source,1,sizeof(byte_copy_source)-1,file)==sizeof(byte_copy_source)-1 && !fclose(file));return 0;}
 if(argc==2 && !strcmp(argv[1],"--compiler-controls")){const XrXirSourceText input={NULL,byte_copy_source,sizeof(byte_copy_source)-1};byte_compiler_resources(&input,false);return 0;}
 bool emit=argc==4 && !strcmp(argv[1],"--emit-native");CHECK(argc==1 || emit);
 if(!emit){byte_direct_cases();byte_source_refusals();byte_consumer(byte_baseline,false,NULL,NULL);puts("Independent U8 bytes including NUL/multibyte oracle and receiving baseline PASS");}
 byte_consumer(byte_copy_source,true,emit?argv[2]:NULL,emit?argv[3]:NULL);puts("Ordinary dynamic/folded/empty/isolated UTF-8 copies and independent value after producer/Program/ledger death PASS");return 0;
}
