/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_regex_precompile.c - Ordinary source-free regex program consumer
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_source_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_array.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_emit_c.h"
#include <ctype.h>
static const char *patterns[]={"","a","ab*","a*b*","ab*a"};
static const char *texts[]={"","a","b","ab","abb","aaabbb","aba","abba","bb","c","ac"};
static const char regex_user[]=
 "import {compileRegex,matchesRegex} from \"./regex\"\n"
 "export fn program()->Array<i64>{return comptime compileRegex(\"ab*\")}\n"
 "fn makeRegexPrograms()->Array<Array<i64>>{return [compileRegex(\"\"),compileRegex(\"a\"),compileRegex(\"ab*\"),compileRegex(\"a*b*\"),compileRegex(\"ab*a\")]}\n"
 "export fn matrix()->Array<bool>{\n"
 " const programs=comptime makeRegexPrograms()\n"
 " const texts:Array<string>=[\"\",\"a\",\"b\",\"ab\",\"abb\",\"aaabbb\",\"aba\",\"abba\",\"bb\",\"c\",\"ac\"]\n"
 " var out:Array<bool>=[];var p:i64=0\n"
 " while(p<len(programs)){var i:i64=0;while(i<len(texts)){out.push(matchesRegex(programs[p],texts[i]));i=i+1};p=p+1}\n"
 " return out\n}\n"
 "export fn answer()->i64{return comptime {const code=compileRegex(\"a*b*\");matchesRegex(code,\"aaabbb\") && !matchesRegex(code,\"abba\") ? 42 : 0}}\n";
static bool regex_reference(const char *pattern,const char *text) {
 if(!*pattern)return !*text;
 bool equal=*text && *text==*pattern;
 if(pattern[1]=='*')return regex_reference(pattern+2,text) || (equal && regex_reference(pattern,text+1));
 return equal && regex_reference(pattern+1,text+1);
}
static uint32_t regex_function(const XrXirModule *module,const char *name) {
 uint32_t found=UINT32_MAX;size_t length=strlen(name);
 for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)){CHECK(found==UINT32_MAX);found=f;}
 CHECK(found!=UINT32_MAX);return found;
}
static XrXirStatus regex_source(const XrXirCompileContext *context,const char *text,
 const XrXirLibraryCatalog *catalog,const XrXirSourceComptimeLimits *limits,XrXirSourceResult *output,XrXirSourceDiagnostic *report) {
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
 char path[2048],bytes[16384];size_t length=strlen(text);CHECK(length<sizeof(bytes));memcpy(bytes,text,length+1);
 const char *logical=catalog?"user.xr":"regex.xr";int n=snprintf(path,sizeof(path),"%s/%s",XR_REGEX_PRECOMPILE_WORK,logical);CHECK(n>0 && n<(int)sizeof(path));
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_REGEX_PRECOMPILE_WORK};
 XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,catalog?XR_XIR_PROGRAM:XR_XIR_LIBRARY,catalog,limits};
 XrXirSourceText source={logical,bytes,length};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"Ordinary regex Source status%u %d:%d %s\n",(unsigned)status,diagnostic.line,diagnostic.column,diagnostic.message);
 if(report)*report=diagnostic;xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(path,0xa5,sizeof(path));return status;
}
#include "xir_regex_precompile_negative.h"
static void regex_value(const XrXirValue *value,unsigned which) {
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
static void regex_execute(const XrXirCompileContext *context,XrXirSourceResult *source,const char *emit,const char *packet_path) {
 const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);const XrXirFunction *fold=&module->functions[regex_function(module,"program")];
 for(uint32_t i=0;i<fold->instruction_count;++i)CHECK(fold->instructions[i].op!=XR_XIR_CALL && fold->instructions[i].op!=XR_XIR_CALL_INDIRECT);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(source);
 if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
 XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
 memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_artifact_verify(received,NULL)==XR_XIR_OK);
 CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
 const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
 module=xr_xir_compile_artifact_module(lowered);uint32_t entries[]={regex_function(module,"matrix"),regex_function(module,"program"),regex_function(module,"answer")};
 if(emit){XrXirCSource output={0};CHECK(xr_xir_compile_emit_c(lowered,"fixture_regex",8388608,&output)==XR_XIR_OK);
  for(size_t i=0;i<output.length;++i)if(output.text[i]=='('){size_t next=i+1;while(next<output.length && isspace((unsigned char)output.text[next]))++next;CHECK(next==output.length || output.text[next]!='{');}
  FILE *file=fopen(emit,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length && fprintf(file,"\nconst uint32_t fixture_regex_selection[3]={%uu,%uu,%uu};\n",entries[0],entries[1],entries[2])>0 && !fclose(file));xr_xir_compile_c_source_free(&output);
 }
 XrXirProgram *program=NULL;XrXirStatus sealed=xr_xir_compile_vm_program_take(&lowered,&program);
 XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context->resources,&spent)==XR_COMPILE_RESOURCE_OK);
 fprintf(stderr,"Regex Program status%u allocated%llu peak%llu work%llu\n",(unsigned)sealed,(unsigned long long)spent.allocated_bytes,(unsigned long long)spent.peak_bytes,(unsigned long long)spent.work);
 CHECK(sealed==XR_XIR_OK && !lowered);XrXirValue values[3]={{0}};
 for(unsigned i=0;i<3;++i){XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
  CHECK(xr_xir_instance_start(instance,entries[i],NULL,0)==XR_XIR_CALL_READY);XrXirCallStatus status;unsigned polls=0;do{CHECK(++polls<=4096);status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;}while(status==XR_XIR_CALL_READY);
  CHECK(status==XR_XIR_CALL_RETURNED && xr_xir_instance_take_result(instance,&values[i])==XR_XIR_CALL_RETURNED);regex_value(&values[i],i);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 }
 xr_xir_compile_program_drop(program);xr_compile_resources_release(context->resources);
 for(unsigned i=0;i<3;++i){regex_value(&values[i],i);xr_xir_value_drop(&values[i]);}
}
int main(int argc,char **argv) {
 bool emit=argc==4 && !strcmp(argv[1],"--emit-native");CHECK(argc==1 || emit);
 XrXirCompileContext storage={0};const XrCompileResourceLimits caps={67108864,8388608,128000000};
 CHECK(xr_compile_resources_new(&caps,&storage.resources)==XR_COMPILE_RESOURCE_OK);storage.limits=xr_xir_compile_default_limits();const XrXirCompileContext *context=&storage;
 char library[16384];FILE *file=fopen(XR_REGEX_PRECOMPILE_SOURCE,"rb");CHECK(file);size_t length=fread(library,1,sizeof(library)-1,file);CHECK(!ferror(file) && feof(file) && !fclose(file));library[length]=0;
 XrXirSourceResult producer={0};CHECK(regex_source(context,library,NULL,NULL,&producer,NULL)==XR_XIR_OK);memset(library,0xa5,sizeof(library));
  XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&producer);
 if(!emit)regex_refusals(&packet);
 char memory_namespace[]="xray-core-assertions-v1";
 XrXirLibraryModuleInput binding[2]={{{XR_MODULE_IDENTITY_SCRIPT,NULL,XR_REGEX_PRECOMPILE_WORK},"regex.xr"},{{XR_MODULE_IDENTITY_MEMORY,memory_namespace,NULL},NULL}};XrXirLibraryInput input={packet.bytes,packet.length,{0},binding,2};xr_sha256(packet.bytes,packet.length,input.sha256);
 XrXirLibraryCatalog *catalog=NULL;XrXirStatus bound=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);if(bound!=XR_XIR_OK)fprintf(stderr,"Regex catalog status%u\n",(unsigned)bound);CHECK(bound==XR_XIR_OK);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);memset(&input,0xa5,sizeof(input));memset(&binding,0xa5,sizeof(binding));memset(memory_namespace,0xa5,sizeof(memory_namespace));
 size_t modules=0;const XrModuleResourceBinding *owned=xr_xir_compile_library_catalog_resources_v2(catalog,&modules);
 CHECK(modules==2 && owned && owned[1].authority.kind==XR_MODULE_IDENTITY_MEMORY && !owned[1].logical_path && !owned[1].source_locator && !owned[1].authority.physical_root && !strcmp(owned[1].authority.namespace_id,"xray-core-assertions-v1"));
 XrXirSourceResult source={0};CHECK(regex_source(context,regex_user,catalog,NULL,&source,NULL)==XR_XIR_OK);xr_xir_compile_library_catalog_free(catalog);
 regex_execute(context,&source,emit?argv[2]:NULL,emit?argv[3]:NULL);
 CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
 puts("Ordinary regex subset: source-free compiler and matcher, 48-byte program, 55 independent matches, comptime answer, both Source constructions, packet death and value after Program/ledger caller death; physical0");return 0;
}
