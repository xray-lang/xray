/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_dylib_source.c - Actual Dylib source through cold ownership and VM
 *
 * KEY CONCEPT:
 *   Native authority comes from the real canonical sys identity. Retired Source
 *   owners and overwritten packet storage cannot be borrowed by execution.
 *   Selecting the actual Dylib and Path classes is a family fixture, not a
 *   claim that either complete standard module has passed.
 */
#define XR_DYLIB_SOURCE 1
#include "dylib_runtime.h"
static uint32_t dylib_named(const XrXirModule *module,const char *name){
 for(uint32_t f=0;f<module->function_count;++f)if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
 CHECK(false);return UINT32_MAX;
}
static char *dylib_read(const char *path){
 FILE *file=fopen(path,"rb");CHECK(file&&!fseek(file,0,SEEK_END));long count=ftell(file);CHECK(count>0&&!fseek(file,0,SEEK_SET));
 char *text=xr_malloc((size_t)count+1);CHECK(text&&fread(text,1,(size_t)count,file)==(size_t)count&&!fclose(file));text[count]=0;return text;
}
static char *dylib_source_text(size_t *length){
 char *original=dylib_read(XR_DYLIB_ROOT "/stdlib/sys/sys.xr");const char *start=strstr(original,"export class Dylib {");CHECK(start);
 static const char imports[]="import {Path} from path\n";size_t count=strlen(start);
 CHECK(count<=SIZE_MAX-sizeof(imports)-sizeof(dylib_wrappers));size_t total=sizeof(imports)-1+count+sizeof(dylib_wrappers);
 char *text=xr_malloc(total);CHECK(text);memcpy(text,imports,sizeof(imports)-1);memcpy(text+sizeof(imports)-1,start,count);
 memcpy(text+sizeof(imports)-1+count,dylib_wrappers,sizeof(dylib_wrappers));*length=total-1;
 memset(original,0xa5,strlen(original));xr_free(original);return text;
}
static char *dylib_path_text(void){
 char *original=dylib_read(XR_DYLIB_ROOT "/stdlib/path/path.xr");const char *start=strstr(original,"export class Path {");
 const char *end=start?strstr(start,"\nexport class PathInfo {"):NULL;CHECK(start&&end&&end>start);
 size_t count=(size_t)(end-start);char *text=xr_malloc(count+1);CHECK(text);memcpy(text,start,count);text[count]=0;
 memset(original,0xa5,strlen(original));xr_free(original);return text;
}
static XrXirStatus dylib_check(const XrXirCompileContext *context,const char *text,size_t length,bool genuine,XrXirSourceResult *output,XrXirSourceDiagnostic *observed){
 const char *root=XR_DYLIB_ROOT "/stdlib";XrModuleIdentityAuthority authority={genuine?XR_MODULE_IDENTITY_STDLIB:XR_MODULE_IDENTITY_MEMORY,"sys",genuine?root:NULL};
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
 XrXirSourceRequest request={session,genuine?XR_DYLIB_ROOT "/stdlib/sys/sys.xr":NULL,&authority,context,root,NULL,XR_XIR_PROGRAM,NULL};
 XrXirSourceDiagnostic diagnostic={0};XrXirStatus status;
 if(genuine){
  char *path=dylib_path_text();XrModuleOverlayInput overlay[]={
   {{XR_MODULE_IDENTITY_STDLIB,"path",root},"path/path.xr",XR_DYLIB_ROOT "/stdlib/path/path.xr",path,strlen(path)},
   {authority,"sys/sys.xr",XR_DYLIB_ROOT "/stdlib/sys/sys.xr",text,length}};
  status=xr_xir_compile_source_check_overlay(&request,overlay,2,output,&diagnostic,NULL);memset(path,0xa5,strlen(path));xr_free(path);
 }else{XrXirSourceText source={NULL,text,length};status=xr_xir_compile_source_check_text(&request,&source,output,&diagnostic,NULL);}
 if(status!=XR_XIR_OK)fprintf(stderr,"Dylib Source%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
 if(observed)*observed=diagnostic;
 xr_compile_session_free(session);return status;
}
static void dylib_contracts(const XrXirCompileContext *context,const XrXirArtifact *code){
 const char *names[]={"__dylibOpen","__dylibSymbol","__dylibClose","__dylibLastError"};
 const char *ctfe[]={"ctfeOpen","ctfeSymbol","ctfeClose","ctfeError"};const XrXirModule *module=xr_xir_compile_artifact_module(code);
 for(unsigned n=0;n<4;++n){
  uint32_t f=dylib_named(module,names[n]);CHECK(module->declarations->functions[f].native_callable==104+n&&xir_native_callable_verify(module,f)==XR_XIR_OK);
  XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=dylib_named(module,ctfe[n]),.target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
   .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
  memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue result={0};
  CHECK(xr_xir_compile_ctfe_calls(&request,&result)==XR_XIR_CTFE_NOT_SUPPORTED&&!result.type&&!result.payload&&!result.reserved);
 }
 static const char *const invalid[]={
  "import {Path} from path\nexport fn wrong()->bool{var f=__dylibClose;return f(1)}\n",
  "import {Path} from path\nexport fn wrong()->bool{var h=1;return __dylibClose(ref h)}\n",
  "import {Path} from path\nexport fn wrong(p:Ptr<u8>){unsafe{p[0]=1}}\n",
  "export fn wrong(p:Path)->i64{return __dylibOpen(p)}\n",
  "import {Path} from path\nclass Fake{raw:string\ninit(raw:string){this.raw=raw}}\nexport fn wrong(p:Fake)->i64{return __dylibOpen(p)}\n"
 };
 for(unsigned n=0;n<sizeof(invalid)/sizeof(*invalid);++n){
  XrXirSourceResult output={0};CHECK(dylib_check(context,invalid[n],strlen(invalid[n]),true,&output,NULL)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
 }
 static const char ordinary_defer[]="\nexport fn ordinaryDefer(lib:Dylib){defer{lib.close()}}\n";
 size_t length=0;char *body=dylib_source_text(&length);CHECK(length<=SIZE_MAX-sizeof(ordinary_defer));
 char *negative=xr_malloc(length+sizeof(ordinary_defer));CHECK(negative);memcpy(negative,body,length);memcpy(negative+length,ordinary_defer,sizeof(ordinary_defer));
 xr_free(body);XrXirSourceResult rejected={0};XrXirSourceDiagnostic diagnostic={0};
 CHECK(dylib_check(context,negative,length+sizeof(ordinary_defer)-1,true,&rejected,&diagnostic)==XR_XIR_BAD_TYPE&&
  !rejected.checked&&!rejected.snapshot&&strstr(diagnostic.message,"E0392:")&&diagnostic.line>0&&diagnostic.column>=0);
 memset(negative,0xa5,length+sizeof(ordinary_defer));xr_free(negative);xr_xir_compile_source_result_free(&rejected);
 const char *impostor="export fn wrong()->string{return __dylibLastError()}\n";XrXirSourceResult output={0};
 CHECK(dylib_check(context,impostor,strlen(impostor),false,&output,NULL)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
}
static void dylib_emit(const XrXirArtifact *code,const uint32_t *ids,const char *path,bool write){
 XrXirCSource source={0};CHECK(xr_xir_compile_emit_c(code,"dylib_runtime",16777216,&source)==XR_XIR_OK);
 char tail[512];int count=snprintf(tail,sizeof(tail),"\nconst uint32_t dylib_runtime_ids[%u]={",DYLIB_ENTRY_COUNT);CHECK(count>0);
 for(unsigned n=0;n<DYLIB_ENTRY_COUNT;++n){int added=snprintf(tail+count,sizeof(tail)-(size_t)count,"%s%u",n?",":"",ids[n]);CHECK(added>0);count+=added;}
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
 size_t length=0;char *text=dylib_source_text(&length);XrXirSourceResult source={0};CHECK(dylib_check(&context,text,length,true,&source,NULL)==XR_XIR_OK);memset(text,0xa5,length);xr_free(text);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
 XrXirArtifact *cold=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);
 memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
 CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
 const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t ids[DYLIB_ENTRY_COUNT]={module->declarations->entry_function};
 for(unsigned n=1;n<DYLIB_ENTRY_COUNT;++n)ids[n]=dylib_named(module,dylib_names[n]);
 char generated[2048];CHECK(snprintf(generated,sizeof(generated),"%s/dylib_runtime.c",emit?argv[2]:XR_DYLIB_GENERATED)>0);dylib_emit(lowered,ids,generated,emit);
 if(emit)xr_xir_compile_artifact_free(lowered);
 else{dylib_contracts(&context,lowered);XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_vm_program_take(&lowered,&program);
  if(status!=XR_XIR_OK)fprintf(stderr,"Dylib VM program take status%u\n",status);
  CHECK(status==XR_XIR_OK&&!lowered);dylib_execute(program,ids);xr_xir_compile_program_drop(program);}
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes&&!runtime_live&&!runtime_bytes);xr_compile_resources_release(context.resources);
 if(!emit)puts("42");return 0;
}
