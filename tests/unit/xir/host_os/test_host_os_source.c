/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_host_os_source.c - Cold canonical host declarations and actual VM calls
 */
#include "host_os_runtime.h"
#include "../xir_genuine_sys_path.h"
#include "host_os_source_cases.h"
#include "host_os_path_cases.h"
#include "host_os_cpu_cases.h"
#include "host_os_system_cases.h"
#include "host_os_stat_cases.h"
#include "host_os_io_bool_cases.h"
#include "host_os_io_links_cases.h"
#include "host_os_io_temp_cases.h"
#include "host_os_io_directory_cases.h"
#define XR_HOST_OS_FILE_PREPARE_SOURCE 1
#include "host_os_io_file_prepare_cases.h"
#include "host_os_signal_frontier_cases.h"
#include "host_os_thread_token_cases.h"
static uint32_t host_named(const XrXirModule *module,const char *name){
 for(uint32_t f=0;f<module->function_count;++f)
  if(module->functions[f].name_length==strlen(name)&&!memcmp(module->functions[f].name,name,strlen(name)))return f;
 CHECK(false);return UINT32_MAX;
}
static XrXirStatus host_check(const XrXirCompileContext *context,const char *module,
 const char *text,bool genuine,XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic){
 const char *root=XR_HOST_OS_ROOT "/stdlib";char path[2048],logical[64];
 CHECK(snprintf(logical,sizeof(logical),"%s/%s.xr",module,module)>0&&snprintf(path,sizeof(path),"%s/%s",root,logical)>0);
 XrModuleIdentityAuthority authority=genuine?(XrModuleIdentityAuthority){XR_MODULE_IDENTITY_STDLIB,module,root}:
  (XrModuleIdentityAuthority){XR_MODULE_IDENTITY_MEMORY,"wrong",NULL};
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
 XrXirSourceRequest request={session,genuine?path:NULL,&authority,context,root,NULL,XR_XIR_PROGRAM,NULL};XrXirStatus status;
 if(!strcmp(module,"io")&&genuine){
  char locator[2048];CHECK(snprintf(locator,sizeof(locator),"%s/path/path.xr",root)>0);
  XrModuleOverlayInput overlays[]={{{XR_MODULE_IDENTITY_STDLIB,"path",root},"path/path.xr",locator,host_path_text,strlen(host_path_text)},
   {authority,logical,path,text,strlen(text)}};
  status=xr_xir_compile_source_check_overlay(&request,overlays,2,output,diagnostic,NULL);
 }else if(!strcmp(module,"sys")&&genuine){
  status=xir_genuine_sys_path_check(&request,text,strlen(text),output,diagnostic);
 }else{
  XrXirSourceText input={genuine?logical:NULL,text,strlen(text)};
  status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
 }
 xr_compile_session_free(session);return status;
}
static XrXirArtifact *host_code(XrXirCompileContext *context,const char *module,const char *text){
 XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirStatus status=host_check(context,module,text,true,&source,&diagnostic);
 if(status!=XR_XIR_OK)fprintf(stderr,"%s Source%u %d:%d %s\n",module,status,diagnostic.line,diagnostic.column,diagnostic.message);
 CHECK(status==XR_XIR_OK);XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
 xr_xir_compile_source_result_free(&source);XrXirArtifact *cold=NULL,*closed=NULL,*code=NULL;
 CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&cold,NULL)==XR_XIR_OK);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
 CHECK(xr_xir_compile_specialize(cold,&closed,NULL,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(cold);
 CHECK(xr_xir_compile_lower(closed,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},NULL,&code,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);return code;
}
static XrXirProgram *host_replica(const XrXirCompileContext *context,const XrXirProgram *source){
 const XrXirArtifact *actual=source->permissions->input->artifact;XrXirProgramPacket packet=xir_program_packet_borrow(actual);
 CHECK(packet.bytes&&packet.identity&&packet.length);XrXirArtifact *copy=NULL;
 CHECK(xr_xir_compile_checked_read_lower(context,packet.bytes,packet.length,&actual->target,&copy,NULL)==XR_XIR_OK);
 XrXirProgram *out=NULL;CHECK(xr_xir_compile_vm_program_take(&copy,&out)==XR_XIR_OK&&!copy);return out;
}
#if defined(XR_OS_WINDOWS)
static XrXirProgram *host_directory_replica(const XrXirCompileContext *context,const XrXirProgram *source){return host_replica(context,source);}
#endif
static void host_emit(const XrXirArtifact *code,const char *prefix,const uint32_t *ids,unsigned count,const char *directory,bool write){
 XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(code,prefix,16777216,&c)==XR_XIR_OK);char path[2048],tail[256];
 CHECK(snprintf(path,sizeof(path),"%s/%s.c",directory,prefix)>0);
 int length=snprintf(tail,sizeof(tail),"\nconst uint32_t %s_ids[%u]={",prefix,count);CHECK(length>0);
 for(unsigned n=0;n<count;++n){int add=snprintf(tail+length,sizeof(tail)-(size_t)length,"%s%u",n?",":"",ids[n]);CHECK(add>0);length+=add;}
 CHECK((size_t)length+4<sizeof(tail));memcpy(tail+length,"};\n",4);length+=3;
 FILE *file=fopen(path,write?"wb":"rb");CHECK(file);
 if(write)CHECK(fwrite(c.text,1,c.length,file)==c.length&&fwrite(tail,1,(size_t)length,file)==(size_t)length);
 else{for(size_t i=0;i<c.length;++i)CHECK(fgetc(file)==(unsigned char)c.text[i]);for(int i=0;i<length;++i)CHECK(fgetc(file)==(unsigned char)tail[i]);CHECK(fgetc(file)==EOF&&!ferror(file));}
 CHECK(!fclose(file));xr_xir_compile_c_source_free(&c);
}
static void host_ctfe(const XrXirArtifact *code,uint32_t function,const XrXirValue *argument,uint32_t count){
 XrXirCtfeCallRequest request={.evaluation={.artifact=code,.function=function,.target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
  .arguments=argument,.argument_count=count,.limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
 memcpy(request.evaluation.checked_identity,code->checked_identity,32);XrXirValue value={0};
 CHECK(xr_xir_compile_ctfe_calls(&request,&value)==XR_XIR_CTFE_NOT_SUPPORTED&&!value.type&&!value.payload);
}
#include "host_os_io_file_prepare_contracts.h"
static void host_io_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const XrXirModule *m=xr_xir_compile_artifact_module(code);uint32_t f=host_named(m,"__chdir"),field=0;XrXirType path=m->functions[f].parameters[0];
 CHECK(xir_native_callable_verify(m,f)==XR_XIR_OK&&xir_native_callable_verify(m,host_named(m,"__chmod"))==XR_XIR_OK);
 CHECK(xir_io_path_field(m->types,path,&field)&&field==1);host_ctfe(code,host_named(m,"ctfeDir"),NULL,0);host_ctfe(code,host_named(m,"ctfeMode"),NULL,0);
 for(unsigned n=0;n<sizeof(host_io_controls)/sizeof(*host_io_controls);++n){
  XrXirSourceResult output={0};XrXirSourceDiagnostic d={0};CHECK(host_check(context,"io",host_io_controls[n],true,&output,&d)==host_io_statuses[n]);
  if(host_io_statuses[n]!=XR_XIR_OK)CHECK(!output.checked&&!output.snapshot&&d.status==host_io_statuses[n]);
  if(n==1)CHECK(strstr(d.message,"view lifetime normalization")&&strstr(d.message,"(CALL)")&&d.line==2);
  if(n==4)CHECK(strstr(d.message,"projected view root has not completed Source admission"));
  xr_xir_compile_source_result_free(&output);
 }
 XrXirModule changed=*m;XrXirDeclarations scope=*m->declarations;XrXirSourceModule modules[16];CHECK(scope.module_count<16);
 memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));scope.modules=modules;changed.declarations=&scope;uint32_t owner=scope.functions[f].module;
 modules[owner].dependencies=NULL;modules[owner].dependency_count=0;
 CHECK(xr_xir_compile_nominal_access(context,&changed,f,path,0,XR_XIR_NOMINAL_TYPE)==XR_XIR_OK);
 CHECK(xr_xir_compile_nominal_access(context,&changed,f,path,field,XR_XIR_NOMINAL_READ)==XR_XIR_BAD_TYPE&&xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
 modules[owner]=m->declarations->modules[owner];modules[owner].name="io";modules[owner].name_length=2;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);modules[owner]=m->declarations->modules[owner];
 XrXirTypes types=*m->types;XrXirNominalTable table=*types.nominals;types.nominals=&table;changed.types=&types;
 const XrXirTypeNode *node=xr_xir_type_node(&types,path);CHECK(node&&table.count<64&&node->nominal.field_count<16);
 XrXirNominalDeclaration declarations[64];XrXirNominalIdentity identities[64];XrXirNominalField fields[16];XrXirNominalFieldIdentity names[16];
 if(table.declarations){memcpy(declarations,table.declarations,table.count*sizeof(*declarations));table.declarations=declarations;memcpy(fields,declarations[node->nominal.declaration].fields,node->nominal.field_count*sizeof(*fields));declarations[node->nominal.declaration].fields=fields;fields[field].flags|=XR_XIR_FIELD_PRIVATE;}
 else{CHECK(table.identities);memcpy(identities,table.identities,table.count*sizeof(*identities));table.identities=identities;memcpy(names,identities[node->nominal.declaration].fields,node->nominal.field_count*sizeof(*names));identities[node->nominal.declaration].fields=names;names[field].flags|=XR_XIR_FIELD_PRIVATE;}
 CHECK(!xir_io_path_field(&types,path,&field)&&xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
 if(table.declarations)fields[1].flags=XR_XIR_FIELD_PROTECTED;else names[1].flags=XR_XIR_FIELD_PROTECTED;
 CHECK(!xir_io_path_field(&types,path,&field)&&xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
 if(table.declarations)fields[1].flags=0;else names[1].flags=0;
 XrXirTypeNode nodes[128];CHECK(types.count<128);memcpy(nodes,types.nodes,types.count*sizeof(*nodes));types.nodes=nodes;
 XrXirType raw[16];memcpy(raw,node->nominal.fields,node->nominal.field_count*sizeof(*raw));
 uint32_t index=(uint32_t)path-XR_XIR_CONSTRUCTED_TYPE_BASE;nodes[index].nominal.fields=raw;
 raw[1]=XR_XIR_I64;CHECK(!xir_io_path_field(&types,path,&field)&&xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
 nodes[types.count]=(XrXirTypeNode){.kind=XR_XIR_TYPE_MUT_SLICE,.element=XR_XIR_U8};raw[1]=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+types.count);++types.count;
 CHECK(!xir_io_path_field(&types,path,&field)&&xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
}
static void host_cpu_contracts(XrXirCompileContext *context,const XrXirArtifact *code,bool pin){
 const char *module=pin?"sys":"os",*name=pin?"__pinToCpu":"__cpuCount";XrXirSourceResult output={0};XrXirSourceDiagnostic d={0};
 CHECK(host_check(context,module,pin?"__pinToCpu(0)":"__cpuCount()",false,&output,&d)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
 CHECK(host_check(context,module,pin?"export fn answer()->bool{return __pinToCpu()}":"export fn answer()->i64{return __cpuCount(1)}",true,&output,&d)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
 CHECK(host_check(context,module,pin?"fn __pinToCpu(cpu:i64)->bool{return false}":"fn __cpuCount()->i64{return 7}",true,&output,&d)==XR_XIR_OK);
 const XrXirModule *shadow=xr_xir_compile_artifact_module(output.checked);CHECK(!shadow->declarations->functions[host_named(shadow,name)].native_callable);xr_xir_compile_source_result_free(&output);
 const XrXirModule *m=xr_xir_compile_artifact_module(code);uint32_t f=host_named(m,name);CHECK(xir_native_callable_verify(m,f)==XR_XIR_OK);
 XrXirValue argument={XR_XIR_I64,0,0};host_ctfe(code,f,pin?&argument:NULL,pin?1u:0u);
 XrXirModule changed=*m;XrXirDeclarations scope=*m->declarations;XrXirFunction functions[64];XrXirFunctionIdentity identities[64];XrXirSourceModule modules[16];CHECK(m->function_count<64&&scope.module_count<16);
 memcpy(functions,m->functions,m->function_count*sizeof(*functions));memcpy(identities,scope.functions,m->function_count*sizeof(*identities));memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));changed.functions=functions;changed.declarations=&scope;scope.functions=identities;scope.modules=modules;uint32_t owner=identities[f].module;
 modules[owner].name=module;modules[owner].name_length=(uint32_t)strlen(module);CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);modules[owner]=m->declarations->modules[owner];functions[f].result=XR_XIR_STRING;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);functions[f]=m->functions[f];identities[f].promises=0;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_STRUCTURE);
}
static void host_system_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const char *names[]={"__uid","__gid","__ppid","__kill","__totalMemory","__freeMemory","__uptime","__loadavg"};
 const char *shadows[]={"fn __uid()->i64?{return null}","fn __gid()->i64?{return null}",
  "fn __ppid()->i64{return 7}","fn __kill(pid:i64,signal:i64)->bool{return false}",
  "fn __totalMemory()->i64{return 7}","fn __freeMemory()->i64{return 7}",
  "fn __uptime()->f64{return 0.0}","fn __loadavg()->Array<f64>{return []}"};
 const XrXirModule *m=xr_xir_compile_artifact_module(code);
 for(unsigned n=0;n<8;++n){
  uint32_t f=host_named(m,names[n]);CHECK(xir_native_callable_verify(m,f)==XR_XIR_OK);
  XrXirValue args[]={{XR_XIR_I64,0,1},{XR_XIR_I64,0,0}};host_ctfe(code,f,n==3?args:NULL,n==3?2u:0u);
  XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};char text[128];
  CHECK(snprintf(text,sizeof(text),"%s(%s)",names[n],n==3?"1,0":"")>0);
  CHECK(host_check(context,"os",text,false,&output,&diagnostic)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);
  xr_xir_compile_source_result_free(&output);
  CHECK(snprintf(text,sizeof(text),"%s(%s)",names[n],n==3?"":"1")>0);
  CHECK(host_check(context,"os",text,true,&output,&diagnostic)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);
  xr_xir_compile_source_result_free(&output);
  CHECK(host_check(context,"os",shadows[n],true,&output,&diagnostic)==XR_XIR_OK);
  const XrXirModule *shadow=xr_xir_compile_artifact_module(output.checked);
  CHECK(!shadow->declarations->functions[host_named(shadow,names[n])].native_callable);
  xr_xir_compile_source_result_free(&output);
  XrXirModule changed=*m;XrXirFunction functions[64];XrXirDeclarations scope=*m->declarations;
  XrXirFunctionIdentity identities[64];XrXirSourceModule modules[16];CHECK(m->function_count<64&&scope.module_count<16);
  memcpy(functions,m->functions,m->function_count*sizeof(*functions));changed.functions=functions;
  memcpy(identities,scope.functions,m->function_count*sizeof(*identities));scope.functions=identities;
  memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));scope.modules=modules;changed.declarations=&scope;
  functions[f].result=XR_XIR_STRING;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);functions[f]=m->functions[f];
  uint32_t owner=identities[f].module;modules[owner].name="os";modules[owner].name_length=2;
  CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);modules[owner]=m->declarations->modules[owner];
  identities[f].promises=0;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_STRUCTURE);
 }
}
#include "host_os_stat_contracts.h"
#include "host_os_io_bool_contracts.h"
#include "host_os_thread_token_contracts.h"
static void host_io_links_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const XrXirModule *module=xr_xir_compile_artifact_module(code);
 const char *names[]={"__symlink","__readlink","__realpath"},*ctfe[]={"ctfeSymlink","ctfeReadlink","ctfeRealpath"};
 for(unsigned n=0;n<3;++n){uint32_t f=host_named(module,names[n]);
  CHECK(module->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_IO_SYMLINK+n&&xir_native_callable_verify(module,f)==XR_XIR_OK);
  host_ctfe(code,host_named(module,ctfe[n]),NULL,0);
  XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};char text[128];
  CHECK(snprintf(text,sizeof(text),"%s()",names[n])>0);
  CHECK(host_check(context,"io",text,false,&output,&diagnostic)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);
  xr_xir_compile_source_result_free(&output);
 }
 uint32_t symlink=host_named(module,"__symlink");XrXirModule changed=*module;XrXirFunction functions[64];CHECK(module->function_count<64);
 memcpy(functions,module->functions,module->function_count*sizeof(*functions));changed.functions=functions;
 XrXirType parameters[2]={functions[symlink].parameters[0],functions[symlink].parameters[1]};functions[symlink].parameters=parameters;
 for(unsigned p=0;p<2;++p){XrXirType saved=parameters[p];parameters[p]=XR_XIR_STRING;
  CHECK(xir_native_callable_verify(&changed,symlink)==XR_XIR_BAD_TYPE);parameters[p]=saved;
 }
 for(unsigned n=0;n<3;++n){uint32_t f=host_named(module,names[n]);XrXirType result=functions[f].result;functions[f].result=XR_XIR_I64;
  CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);functions[f].result=result;
 }
 static const char *const rejected[]={
  "import {Path} from path\nexport fn wrong(p:Path)->bool{return __symlink(42,p)}\n",
  "import {Path} from path\nexport fn wrong(p:Path)->bool{return __symlink(p,42)}\n",
  "export fn noRead(p:Path)->string?{return __realpath(p)}\n",
  "import {Path} from path\nclass Wrong{raw:string\ninit(raw:string){this.raw=raw}}\nexport fn fake(p:Wrong)->string?{return __readlink(p)}\n",
  "import {Path} from path\nexport fn conflict(v:Slice<u8>,a:Path,b:Path)->i64{var changed=__symlink(a,b);return len(v)}\n"
 };
 for(unsigned n=0;n<sizeof(rejected)/sizeof(*rejected);++n){XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
  CHECK(host_check(context,"io",rejected[n],true,&output,&diagnostic)==(n==4?XR_XIR_UNRESOLVED:XR_XIR_BAD_TYPE)&&!output.checked&&!output.snapshot);
  if(n==4)CHECK(strstr(diagnostic.message,"view lifetime normalization")&&strstr(diagnostic.message,"(CALL)"));
  xr_xir_compile_source_result_free(&output);
 }
 const char *alias="import {Path} from path\nfn pair(a:ref Path,b:ref Path)->bool{return __symlink(a,b)}\nexport fn bad()->bool{var p=Path(\".\");return pair(ref p,ref p)}\n";
 XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
 CHECK(host_check(context,"io",alias,true,&output,&diagnostic)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot&&strstr(diagnostic.message,"must not alias"));
 xr_xir_compile_source_result_free(&output);
}
static void host_io_temp_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const XrXirModule *m=xr_xir_compile_artifact_module(code);const char *names[]={"__makeTempDir","__makeTempFile"};
 const char *ctfe[]={"ctfeDirectory","ctfeFile"};
 for(unsigned n=0;n<2;++n){
  uint32_t f=host_named(m,names[n]);CHECK(m->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_IO_MAKE_TEMP_DIR+n&&xir_native_callable_verify(m,f)==XR_XIR_OK);
  host_ctfe(code,host_named(m,ctfe[n]),NULL,0);
  XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};char text[160];
  CHECK(snprintf(text,sizeof(text),"%s(\".\")",names[n])>0);
  CHECK(host_check(context,"io",text,false,&output,&diagnostic)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
  CHECK(snprintf(text,sizeof(text),"import {Path} from path\nexport fn bad(root:i64)->string?{return %s(root)}",names[n])>0);
  CHECK(host_check(context,"io",text,true,&output,&diagnostic)==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
  CHECK(snprintf(text,sizeof(text),"import {Path} from path\nfn %s(root:string)->string?{return null}",names[n])>0);
  XrXirStatus shadow_status=host_check(context,"io",text,true,&output,&diagnostic);
  if(shadow_status!=XR_XIR_OK)fprintf(stderr,"temp shadow %u %s\n",shadow_status,diagnostic.message);
  CHECK(shadow_status==XR_XIR_OK);
  CHECK(!xr_xir_compile_artifact_module(output.checked)->declarations->functions[host_named(xr_xir_compile_artifact_module(output.checked),names[n])].native_callable);
  xr_xir_compile_source_result_free(&output);
  XrXirModule changed=*m;XrXirDeclarations scope=*m->declarations;XrXirSourceModule modules[16];XrXirFunction functions[64];
  CHECK(scope.module_count<16&&m->function_count<64);memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));scope.modules=modules;changed.declarations=&scope;
  memcpy(functions,m->functions,m->function_count*sizeof(*functions));changed.functions=functions;
  uint32_t owner=scope.functions[f].module;modules[owner].name="io";modules[owner].name_length=2;
  CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);modules[owner]=m->declarations->modules[owner];
  modules[owner].lexical_scope=1;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);modules[owner]=m->declarations->modules[owner];
  XrXirType wrong=XR_XIR_I64;functions[f].parameters=&wrong;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);functions[f]=m->functions[f];
  functions[f].result=XR_XIR_STRING;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
 }
}
static void host_io_directory_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const XrXirModule *m=xr_xir_compile_artifact_module(code);const char *names[]={"__dirOpen","__dirNext","__dirClose"};
 const char *ctfe[]={"ctfeOpen","ctfeNext","ctfeClose"};
 const char *shadows[]={"fn __dirOpen(path:Path)->i64{return 0}","fn __dirNext(handle:i64)->string?{return null}","fn __dirClose(handle:i64)->bool{return false}"};
 for(unsigned n=0;n<3;++n){
  uint32_t f=host_named(m,names[n]);CHECK(m->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_IO_DIR_OPEN+n&&xir_native_callable_verify(m,f)==XR_XIR_OK);
  host_ctfe(code,host_named(m,ctfe[n]),NULL,0);XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};char text[192];
  CHECK(snprintf(text,sizeof(text),"%s(1)",names[n])>0);
  CHECK(host_check(context,"io",text,false,&output,&diagnostic)==XR_XIR_BAD_VALUE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
  CHECK(snprintf(text,sizeof(text),"import {Path} from path\n%s\n",shadows[n])>0);
  CHECK(host_check(context,"io",text,true,&output,&diagnostic)==XR_XIR_OK);
  const XrXirModule *shadow=xr_xir_compile_artifact_module(output.checked);CHECK(!shadow->declarations->functions[host_named(shadow,names[n])].native_callable);xr_xir_compile_source_result_free(&output);
  XrXirModule changed=*m;XrXirDeclarations scope=*m->declarations;XrXirSourceModule modules[16];XrXirFunction functions[64];
  CHECK(scope.module_count<16&&m->function_count<64);memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));scope.modules=modules;changed.declarations=&scope;
  memcpy(functions,m->functions,m->function_count*sizeof(*functions));changed.functions=functions;uint32_t owner=scope.functions[f].module;
  modules[owner].name="io";modules[owner].name_length=2;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);modules[owner]=m->declarations->modules[owner];
  modules[owner].lexical_scope=1;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);modules[owner]=m->declarations->modules[owner];
  XrXirType wrong=XR_XIR_STRING;functions[f].parameters=&wrong;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);functions[f]=m->functions[f];
  functions[f].result=n==0?XR_XIR_BOOL:XR_XIR_I64;CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
 }
 static const char *const rejected[]={
  "import {Path} from path\nexport fn wrong()->i64{return __dirOpen(42)}\n",
  "import {Path} from path\nexport fn wrong()->string?{return __dirNext(\"1\")}\n",
  "import {Path} from path\nexport fn wrong()->bool{return __dirClose(\"1\")}\n",
  "export fn noRead(p:Path)->i64{return __dirOpen(p)}\n",
  "import {Path} from path\nclass Wrong{raw:string\ninit(raw:string){this.raw=raw}}\nexport fn fake(p:Wrong)->i64{return __dirOpen(p)}\n",
  "import {Path} from path\nexport fn conflict(v:Slice<u8>,p:Path)->i64{var opened=__dirOpen(p);return len(v)}\n"
 };
 for(unsigned n=0;n<sizeof(rejected)/sizeof(*rejected);++n){XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
  CHECK(host_check(context,"io",rejected[n],true,&output,&diagnostic)==(n==5?XR_XIR_UNRESOLVED:XR_XIR_BAD_TYPE)&&!output.checked&&!output.snapshot);
  if(n==5)CHECK(strstr(diagnostic.message,"view lifetime normalization")&&strstr(diagnostic.message,"(CALL)"));
  xr_xir_compile_source_result_free(&output);
 }
}
static void host_product(XrXirCompileContext *context,unsigned kind,const char *directory,bool emit){
 const char *modules[]={"os","sys","io","os","io","io","os","io","io","sys","io","io"},*texts[]={host_count_text,host_pin_text,host_io_text,host_system_text,host_stat_text,host_io_bool_text,host_signal_frontier_text,host_io_links_text,host_io_temp_text,NULL,host_io_directory_text,host_file_prepare_text},*prefixes[]={"host_count","host_pin","host_path","host_system","host_stat","host_io_bool","host_signal_frontier","host_io_links","host_io_temp","host_thread_tokens","host_io_directory","host_io_file_prepare"};
 char *owned=kind==9?host_thread_token_source():NULL;
 XrXirArtifact *code=host_code(context,modules[kind],owned?owned:texts[kind]);xr_free(owned);const XrXirModule *m=xr_xir_compile_artifact_module(code);
 uint32_t ids[HOST_TOKEN_COUNT]={m->declarations->entry_function};unsigned count=kind==11?HOST_FILE_COUNT:kind==10?6u:kind==9?HOST_TOKEN_COUNT:kind==8?4u:kind==7?6u:kind==6?5u:kind==5?7u:kind==4?4u:kind==3?9u:kind==2?5u:2u;
 if(kind==9){const char *names[]={"current","alive","throughValue","basic","retry","recursion","panicked","cancelled","yielded","waited","dynamic","nullable","__threadLocalId","__threadLocalAlive","barrier","invalidBarrier"};for(unsigned n=0;n<16;++n)ids[n+1]=host_named(m,names[n]);}
 else if(kind==3){const char *names[]={"uid","gid","parent","signal","total","available","elapsed","load"};for(unsigned n=0;n<8;++n)ids[n+1]=host_named(m,names[n]);}
 else ids[1]=host_named(m,kind==8?"directory":kind==6?"parent":(kind==2||kind==4||kind==5||kind==7||kind==10||kind==11)?"make":"answer");
 if(kind==11){const char *names[]={"closeFile","flushFile","openFile","openWrite","readFile","writeFile","writeText"};for(unsigned n=0;n<7;++n)ids[HOST_FILE_CLOSE+n]=host_named(m,names[n]);}
 if(kind==10){const char *names[]={"openPath","nextName","closePath","byRef"};for(unsigned n=0;n<4;++n)ids[n+2]=host_named(m,names[n]);}
 if(kind==8){ids[2]=host_named(m,"file");ids[3]=host_named(m,"nested");}
 if(kind==7){const char *names[]={"symlinkPath","readlinkPath","realpathPath","byRef"};for(unsigned n=0;n<4;++n)ids[n+2]=host_named(m,names[n]);}
 if(kind==6){ids[2]=host_named(m,"failed");ids[3]=host_named(m,"heldParent");ids[4]=host_named(m,"initialized");}
 if(kind==5){const char *names[]={"mkdirPath","rmdirPath","removePath","renamePath","utimePath"};for(unsigned n=0;n<5;++n)ids[n+2]=host_named(m,names[n]);}
 if(kind==4){ids[2]=host_named(m,"query");ids[3]=host_named(m,"snapshot");}
 if(kind==2){ids[2]=host_named(m,"chmodPath");ids[3]=host_named(m,"chdirPath");ids[4]=host_named(m,"viaRef");}
 host_emit(code,prefixes[kind],ids,count,directory,emit);
 if(!emit){if(kind==11){if(!host_file_prepare_child)host_file_prepare_contracts(context,code);}else if(kind==10)host_io_directory_contracts(context,code);else if(kind==9)host_thread_token_contracts(context,code);else if(kind==8)host_io_temp_contracts(context,code);else if(kind==7)host_io_links_contracts(context,code);else if(kind==6){CHECK(!(m->declarations->functions[ids[1]].promises&XR_XIR_FUNCTION_NO_SUSPEND));CHECK(m->declarations->functions[host_named(m,"__cpuCount")].promises&XR_XIR_FUNCTION_NO_SUSPEND);}else if(kind==5)host_io_bool_contracts(context,code);else if(kind==4)host_stat_contracts(context,code);else if(kind==2)host_io_contracts(context,code);else if(kind==3)host_system_contracts(context,code);else host_cpu_contracts(context,code,kind==1);}
 if(emit)xr_xir_compile_artifact_free(code);
 else{XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&code,&program)==XR_XIR_OK&&!code);if(kind==11){host_file_prepare_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==10){host_io_directory_execute(context,program,ids);xr_xir_compile_program_drop(program);}else if(kind==9){host_thread_tokens_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==8){host_io_temp_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==7){host_io_links_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==6){host_signal_frontier_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==5){host_io_bool_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==4){host_stat_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==3){host_system_execute(program,ids);xr_xir_compile_program_drop(program);}else if(kind==2){host_path_execute(program,ids);host_path_receiving(context,&program,ids);CHECK(!program);}else{host_cpu_execute(program,ids,kind==1);xr_xir_compile_program_drop(program);}}
}
int main(int argc,char **argv){
#if defined(XR_OS_WINDOWS)
 CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
 host_file_prepare_child=argc==2&&!strcmp(argv[1],"--file-prepare-stdio");host_file_prepare_executable=argv[0];
#if defined(XR_OS_WINDOWS)
 if(host_file_prepare_child)CHECK(_setmode(_fileno(stdin),_O_BINARY)!=-1&&_setmode(_fileno(stderr),_O_BINARY)!=-1);
#endif
 bool emit=argc==3&&!strcmp(argv[1],"--emit");CHECK(emit||argc==1||host_file_prepare_child);if(!emit&&!host_file_prepare_child)host_cpu_concurrency();
 XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
 XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
 if(host_file_prepare_child)host_product(&context,11,XR_HOST_OS_GENERATED,false);
 else for(unsigned kind=0;kind<12;++kind)host_product(&context,kind,emit?argv[2]:XR_HOST_OS_GENERATED,emit);
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes&&!runtime_live&&!runtime_bytes);
 xr_compile_resources_release(context.resources);if(!emit){CHECK(xr_os_io_thread_token_shutdown()==XR_OS_IO_OK);if(!host_file_prepare_child)puts("42");}return 0;
}
