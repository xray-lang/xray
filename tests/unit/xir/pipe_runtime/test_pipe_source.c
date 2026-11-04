/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_pipe_source.c - Actual Pipe source through cold ownership and VM
 *
 * KEY CONCEPT:
 *   Native authority comes from the real canonical sys identity. Retired Source
 *   owners and overwritten packet storage cannot be borrowed by execution.
 */
#define XR_PIPE_SOURCE 1
#include "pipe_runtime.h"
#include "../xir_genuine_sys_path.h"
static uint32_t pipe_named(const XrXirModule *module,const char *name){
 for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
 CHECK(false);return UINT32_MAX;
}
static char *pipe_source_text(size_t *length){
 FILE *file=fopen(XR_PIPE_ROOT "/stdlib/sys/sys.xr","rb");CHECK(file&&!fseek(file,0,SEEK_END));long size=ftell(file);CHECK(size>0&&!fseek(file,0,SEEK_SET));
 char *original=xr_malloc((size_t)size+1);CHECK(original&&fread(original,1,(size_t)size,file)==(size_t)size&&!fclose(file));original[size]=0;
 const char *start=strstr(original,"export class Pipe {"),*end=start?strstr(start,"\nexport class ProcessOptions {"):NULL;CHECK(start&&end&&end>start);
 size_t count=(size_t)(end-start);CHECK(count<=SIZE_MAX-sizeof(pipe_wrappers));char *text=xr_malloc(count+sizeof(pipe_wrappers));CHECK(text);
 memcpy(text,start,count);memcpy(text+count,pipe_wrappers,sizeof(pipe_wrappers));*length=count+sizeof(pipe_wrappers)-1;
 memset(original,0xa5,(size_t)size);xr_free(original);return text;
}
static XrXirStatus pipe_check(const XrXirCompileContext *context,const char *text,size_t length,bool genuine,XrXirSourceResult *output){
 const char *root=XR_PIPE_ROOT "/stdlib";XrModuleIdentityAuthority authority={genuine?XR_MODULE_IDENTITY_STDLIB:XR_MODULE_IDENTITY_MEMORY,"sys",genuine?root:NULL};
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
 XrXirSourceRequest request={session,genuine?XR_PIPE_ROOT "/stdlib/sys/sys.xr":NULL,&authority,context,root,NULL,XR_XIR_PROGRAM,NULL};
 XrXirSourceText source={genuine?"sys/sys.xr":NULL,text,length};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=genuine?xir_genuine_sys_path_check(&request,text,length,output,&diagnostic):
  xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);
 if(status!=XR_XIR_OK)fprintf(stderr,"Pipe Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
 xr_compile_session_free(session);return status;
}
static void pipe_contracts(const XrXirCompileContext *context,const XrXirArtifact *code){
 const char *names[]={"__pipeOpen","__pipeRead","__pipeWrite","__pipeClose"};const char *ctfe[]={"ctfeOpen","ctfeRead","ctfeWrite","ctfeClose"};
 const XrXirModule *module=xr_xir_compile_artifact_module(code);
 for(unsigned n=0;n<4;++n){
  uint32_t f=pipe_named(module,names[n]);CHECK(module->declarations->functions[f].native_callable==97+n&&xir_native_callable_verify(module,f)==XR_XIR_OK);
  XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=pipe_named(module,ctfe[n]),.target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
   .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
  memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue result={0};
  CHECK(xr_xir_compile_ctfe_calls(&request,&result)==XR_XIR_CTFE_NOT_SUPPORTED&&!result.type&&!result.payload&&!result.reserved);
 }
 static const char *const invalid[]={
  "export fn wrong()->(i64,i64)?{var f=__pipeOpen;return f()}\n",
  "export fn wrong()->Array<u8>?{var h=1;return __pipeRead(ref h,1)}\n",
  "fn readOnce(h:i64){var ignored=__pipeRead(h,1)}\nexport fn wrong(h:i64){defer{readOnce(h)}}\n",
  "fn writeOnce(h:i64){var data:Array<u8>=[1];var ignored=__pipeWrite(h,data)}\nexport fn wrong(h:i64){defer{writeOnce(h)}}\n"
 };
 for(unsigned n=0;n<sizeof(invalid)/sizeof(*invalid);++n){
  XrXirSourceResult output={0};CHECK(pipe_check(context,invalid[n],strlen(invalid[n]),true,&output)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
 }
 const char *impostor="export fn wrong()->(i64,i64)?{return __pipeOpen()}\n";XrXirSourceResult output={0};
 CHECK(pipe_check(context,impostor,strlen(impostor),false,&output)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
}
static void pipe_emit(const XrXirArtifact *code,const uint32_t *ids,const char *path,bool write){
 XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(code,"pipe_runtime",16777216,&source)==XR_XIR_OK);
 char tail[512];int count=snprintf(tail,sizeof(tail),"\nconst uint32_t pipe_runtime_ids[%u]={",PIPE_ENTRY_COUNT);CHECK(count>0);
 for(unsigned n=0;n<PIPE_ENTRY_COUNT;++n){int added=snprintf(tail+count,sizeof(tail)-(size_t)count,"%s%u",n?",":"",ids[n]);CHECK(added>0);count+=added;}
 CHECK((size_t)count+4<sizeof(tail));memcpy(tail+count,"};\n",4);count+=3;FILE *file=fopen(path,write?"wb":"rb");CHECK(file);
 if(write)CHECK(fwrite(source.text,1,source.length,file)==source.length&&fwrite(tail,1,(size_t)count,file)==(size_t)count);
 else{for(size_t n=0;n<source.length;++n)CHECK(fgetc(file)==(unsigned char)source.text[n]);for(int n=0;n<count;++n)CHECK(fgetc(file)==(unsigned char)tail[n]);CHECK(fgetc(file)==EOF&&!ferror(file));}
 CHECK(!fclose(file));xr_xir_compile_c_source_free(&source);
}
int main(int argc,char **argv){
#if defined(XR_OS_WINDOWS)
 CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
 bool emit=argc==3&&!strcmp(argv[1],"--emit");CHECK(emit||argc==1);
 XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
 CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
 size_t length=0;char *text=pipe_source_text(&length);XrXirSourceResult source={0};CHECK(pipe_check(&context,text,length,true,&source)==XR_XIR_OK);memset(text,0xa5,length);xr_free(text);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
 XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
 memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
 CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
 const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[PIPE_ENTRY_COUNT]={module->declarations->entry_function};
 for(unsigned n=1;n<PIPE_ENTRY_COUNT;++n)ids[n]=pipe_named(module,pipe_names[n]);
 char generated[2048];CHECK(snprintf(generated,sizeof(generated),"%s/pipe_runtime.c",emit?argv[2]:XR_PIPE_GENERATED)>0);pipe_emit(lowered,ids,generated,emit);
 if(emit)xr_xir_compile_artifact_free(lowered);
 else{pipe_contracts(&context,lowered);XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
  if(status!=XR_XIR_OK)fprintf(stderr,"Pipe VM program take status%u\n",status);
  CHECK(status==XR_XIR_OK&&!lowered);pipe_execute(program,ids);xr_xir_compile_program_drop(program);}
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes&&!runtime_live&&!runtime_bytes);xr_compile_resources_release(context.resources);
 if(!emit)puts("42");return 0;
}
