/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_process_source.c - Actual Process source through cold ownership and VM
 *
 * KEY CONCEPT:
 *   Native authority comes from the real canonical sys identity. Retired Source
 *   owners and overwritten packet storage cannot be borrowed by execution.
 */
#define XR_PROCESS_SOURCE 1
#include "process_runtime.h"
static uint32_t process_named(const XrXirModule *module,const char *name){
 for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
 CHECK(false);return UINT32_MAX;
}
static char *process_source_text(size_t *length){
 FILE *file=fopen(XR_PROCESS_ROOT "/stdlib/sys/sys.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0&&!fseek(file,0,SEEK_SET));
 char *original=xr_malloc((size_t)size+1);CHECK(original&&fread(original,1,(size_t)size,file)==(size_t)size&&!fclose(file));original[size]=0;
 const char *signal=strstr(original,"export enum Signal {"),*signal_end=signal?strstr(signal,"\n// KILL cannot be caught"):NULL;
 const char *start=strstr(original,"export class Pipe {"),*end=start?strstr(start,"\nexport enum ThreadLocalError {"):NULL;
 CHECK(signal&&signal_end&&signal_end>signal&&start&&end&&end>start);
 static const char imports[]="import time\n";
 size_t first=(size_t)(signal_end-signal),count=(size_t)(end-start);
 CHECK(first<=SIZE_MAX-count-sizeof(imports)-sizeof(process_wrappers));
 size_t total=sizeof(imports)-1+first+count+sizeof(process_wrappers);
 char *result=xr_malloc(total);CHECK(result);size_t offset=0;
 memcpy(result,imports,sizeof(imports)-1);offset+=sizeof(imports)-1;
 memcpy(result+offset,signal,first);offset+=first;memcpy(result+offset,start,count);offset+=count;
 memcpy(result+offset,process_wrappers,sizeof(process_wrappers));*length=total-1;
 memset(original,0xa5,(size_t)size);xr_free(original);return result;
}

static XrXirStatus process_check(const XrXirCompileContext *context,const char *text,size_t length,bool genuine,XrXirSourceResult *output){
 const char *root=XR_PROCESS_ROOT "/stdlib";XrModuleIdentityAuthority authority={genuine?XR_MODULE_IDENTITY_STDLIB:XR_MODULE_IDENTITY_MEMORY,"sys",genuine?root:NULL};
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
 XrXirSourceRequest request={session,genuine?XR_PROCESS_ROOT "/stdlib/sys/sys.xr":NULL,&authority,context,root,NULL,XR_XIR_PROGRAM,NULL};
 XrXirSourceText source={genuine?"sys/sys.xr":NULL,text,length};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"Process Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
 xr_compile_session_free(session);return status;
}
static void process_contracts(const XrXirCompileContext *context,const XrXirArtifact *code){
 const char *names[]={"__processSpawn","__processTryWait","__processKill"};
 const char *ctfe[]={"ctfeSpawn","ctfeWait","ctfeKill"};
 const XrXirModule *module=xr_xir_compile_artifact_module(code);
 for(unsigned n=0;n<3;++n){
  uint32_t f=process_named(module,names[n]);CHECK(module->declarations->functions[f].native_callable==101+n&&xir_native_callable_verify(module,f)==XR_XIR_OK);
  XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=process_named(module,ctfe[n]),.target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
   .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
  memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue result={0};
  CHECK(xr_xir_compile_ctfe_calls(&request,&result)==XR_XIR_CTFE_NOT_SUPPORTED&&!result.type&&!result.payload&&!result.reserved);
 }
 static const char *const invalid[]={
  "export fn wrong()->i64?{var f=__processTryWait;return f(1)}\n",
  "export fn wrong()->i64?{var h=1;return __processTryWait(ref h)}\n"
 };
 for(unsigned n=0;n<sizeof(invalid)/sizeof(*invalid);++n){
  XrXirSourceResult output={0};CHECK(process_check(context,invalid[n],strlen(invalid[n]),true,&output)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
 }
 const char *impostor="export fn wrong()->i64?{return __processTryWait(1)}\n";XrXirSourceResult output={0};
 CHECK(process_check(context,impostor,strlen(impostor),false,&output)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
}

static void process_emit(const XrXirArtifact *code,const uint32_t *ids,const char *path,bool write){
 XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(code,"process_runtime",16777216,&source)==XR_XIR_OK);
 char tail[512];int count=snprintf(tail,sizeof(tail),"\nconst uint32_t process_runtime_ids[%u]={",PROCESS_ENTRY_COUNT);CHECK(count>0);
 for(unsigned n=0;n<PROCESS_ENTRY_COUNT;++n){int added=snprintf(tail+count,sizeof(tail)-(size_t)count,"%s%u",n?",":"",ids[n]);CHECK(added>0);count+=added;}
 CHECK((size_t)count+4<sizeof(tail));memcpy(tail+count,"};\n",4);count+=3;FILE *file=fopen(path,write?"wb":"rb");CHECK(file);
 if(write)CHECK(fwrite(source.text,1,source.length,file)==source.length&&fwrite(tail,1,(size_t)count,file)==(size_t)count);
 else{for(size_t n=0;n<source.length;++n)CHECK(fgetc(file)==(unsigned char)source.text[n]);for(int n=0;n<count;++n)CHECK(fgetc(file)==(unsigned char)tail[n]);CHECK(fgetc(file)==EOF&&!ferror(file));}
 CHECK(!fclose(file));xr_xir_compile_c_source_free(&source);
}
int main(int argc,char **argv){
#if defined(XR_OS_WINDOWS)
 CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
 if(process_child(argc,argv))return 0;
 bool emit=argc==3&&!strcmp(argv[1],"--emit");CHECK(emit||argc==1);
 XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
 CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
 size_t length=0;char *text=process_source_text(&length);XrXirSourceResult source={0};CHECK(process_check(&context,text,length,true,&source)==XR_XIR_OK);memset(text,0xa5,length);xr_free(text);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
 XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
 memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
 CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
 const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[PROCESS_ENTRY_COUNT]={module->declarations->entry_function};
 for(unsigned n=1;n<PROCESS_ENTRY_COUNT;++n)ids[n]=process_named(module,process_names[n]);
 char generated[2048];CHECK(snprintf(generated,sizeof(generated),"%s/process_runtime.c",emit?argv[2]:XR_PROCESS_GENERATED)>0);process_emit(lowered,ids,generated,emit);
 if(emit)xr_xir_compile_artifact_free(lowered);
 else{process_contracts(&context,lowered);XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
  if(status!=XR_XIR_OK)fprintf(stderr,"Process VM program take status%u\n",status);
  CHECK(status==XR_XIR_OK&&!lowered);process_execute(program,ids);xr_xir_compile_program_drop(program);}
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes&&!runtime_live&&!runtime_bytes);xr_compile_resources_release(context.resources);
 if(!emit)puts("42");return 0;
}
