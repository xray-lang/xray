/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_prepare_contracts.h - Exact file authority and effect replay
 *
 * KEY CONCEPT:
 *   A private cleanup transport is distinct from successful file execution.
 *   Every other file primitive retains its real suspension obligation.
 */
#ifndef HOST_OS_IO_FILE_PREPARE_CONTRACTS_H
#define HOST_OS_IO_FILE_PREPARE_CONTRACTS_H
static void host_file_prepare_rejected(XrXirCompileContext *context,const char *text,bool genuine,
 XrXirStatus expected,const char *reason){
 XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=host_check(context,"io",text,genuine,&output,&diagnostic);
 if(status!=expected)fprintf(stderr,"file preparation contract status%u expected%u %d:%d %s\n",status,expected,diagnostic.line,diagnostic.column,diagnostic.message);
 CHECK(status==expected&&!output.checked&&!output.snapshot&&diagnostic.status==expected);
 if(reason)CHECK(strstr(diagnostic.message,reason));
 xr_xir_compile_source_result_free(&output);
}
static void host_file_prepare_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 static const char *const names[]={"__fileClose","__fileFlush","__fileOpen","__fileOpenWrite","__fileRead","__fileWrite","__fileWriteStr"};
 static const char *const signatures[]={"(handle: i64): bool","(handle: i64): bool","(path: Path): i64","(path: Path, append: bool): i64",
  "(handle: i64, maxBytes: i64): Array<u8>?","(handle: i64, data: Array<u8>, offset: i64): i64","(handle: i64, data: string, offset: i64): i64"};
 static const char *const ctfe[]={"ctfeClose","ctfeFlush","ctfeOpen","ctfeOpenWrite","ctfeRead","ctfeWrite","ctfeWriteStr"};
 static const char *const results[]={"bool","bool","i64","i64","Array<u8>?","i64","i64"};
 static const char *const arguments[]={"3","1","Path(\".\")","Path(\".\"),false","0,7","1,b,0","2,s,0"};
 static const char *const wrong[]={
  "export fn denied()->bool{return __fileClose(\"3\")}",
  "export fn denied()->bool{return __fileFlush(\"1\")}",
  "export fn denied()->i64{return __fileOpen(42)}",
  "export fn denied(p:Path)->i64{return __fileOpenWrite(p,42)}",
  "export fn denied()->Array<u8>?{return __fileRead(0,\"7\")}",
  "export fn denied()->i64{return __fileWrite(1,\"data\",0)}",
  "export fn denied(b:Array<u8>)->i64{return __fileWriteStr(2,b,0)}"};
 const XrXirModule *module=xr_xir_compile_artifact_module(code);CHECK(module&&module->function_count<128&&module->declarations->module_count<16);
 for(unsigned n=0;n<7;++n){
  uint32_t function=host_named(module,names[n]);const XrNativeCallableDeclaration *native=xr_native_callable_by_id(111u+n);
  CHECK(native&&native->member&&!strcmp(native->member->name,names[n])&&!strcmp(native->member->signature,signatures[n]));
  CHECK(module->declarations->functions[function].native_callable==111u+n&&xir_native_callable_verify(module,function)==XR_XIR_OK);
  CHECK(module->declarations->functions[function].promises==(n?0u:XR_XIR_FUNCTION_NO_SUSPEND));
  host_ctfe(code,host_named(module,ctfe[n]),NULL,0);
  XrXirModule changed=*module;XrXirDeclarations scope=*module->declarations;
  XrXirSourceModule modules[16];XrXirFunction functions[128];XrXirFunctionIdentity identities[128];
  memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));scope.modules=modules;changed.declarations=&scope;
  memcpy(functions,module->functions,module->function_count*sizeof(*functions));changed.functions=functions;
  memcpy(identities,scope.functions,module->function_count*sizeof(*identities));scope.functions=identities;
  uint32_t owner=identities[function].module;modules[owner].name="io";modules[owner].name_length=2;
  CHECK(xir_native_callable_verify(&changed,function)==XR_XIR_BAD_TYPE);modules[owner]=module->declarations->modules[owner];
  modules[owner].lexical_scope=1;CHECK(xir_native_callable_verify(&changed,function)==XR_XIR_BAD_TYPE);modules[owner]=module->declarations->modules[owner];
  identities[function].promises^=XR_XIR_FUNCTION_NO_SUSPEND;
  CHECK(xir_native_callable_verify(&changed,function)==XR_XIR_BAD_TYPE);identities[function]=module->declarations->functions[function];
  XrXirType parameters[3]={0};CHECK(functions[function].parameter_count<=3);
  memcpy(parameters,functions[function].parameters,functions[function].parameter_count*sizeof(*parameters));functions[function].parameters=parameters;
  parameters[0]=XR_XIR_STRING;CHECK(xir_native_callable_verify(&changed,function)==XR_XIR_BAD_TYPE);functions[function]=module->functions[function];
  functions[function].result=XR_XIR_UNIT;CHECK(xir_native_callable_verify(&changed,function)==XR_XIR_BAD_TYPE);functions[function]=module->functions[function];
  functions[function].error_type=XR_XIR_UNIT;CHECK(xir_native_callable_verify(&changed,function)==XR_XIR_BAD_TYPE);functions[function]=module->functions[function];
  if(n==2||n==3){
   uint32_t field=UINT32_MAX;XrXirType path=functions[function].parameters[0];CHECK(xir_io_path_field(module->types,path,&field)&&field==1);
   modules[owner].dependencies=NULL;modules[owner].dependency_count=0;
   CHECK(xr_xir_compile_nominal_access(context,&changed,function,path,0,XR_XIR_NOMINAL_TYPE)==XR_XIR_OK);
   CHECK(xr_xir_compile_nominal_access(context,&changed,function,path,field,XR_XIR_NOMINAL_READ)==XR_XIR_BAD_TYPE&&xir_native_callable_verify(&changed,function)==XR_XIR_BAD_TYPE);
  }
  char text[1024];CHECK(snprintf(text,sizeof(text),"import {Path} from path\n%s\n",wrong[n])>0);
  host_file_prepare_rejected(context,text,true,XR_XIR_BAD_TYPE,NULL);
  CHECK(snprintf(text,sizeof(text),"export fn denied(){const value=%s}\n",names[n])>0);
  host_file_prepare_rejected(context,text,false,XR_XIR_BAD_VALUE,NULL);
  CHECK(snprintf(text,sizeof(text),"import {Path} from path\nexport fn denied(p:Path,b:Array<u8>,s:string)->%s{const f=%s;return f(%s)}\n",results[n],names[n],arguments[n])>0);
  host_file_prepare_rejected(context,text,true,XR_XIR_BAD_TYPE,NULL);
  if(n){
   CHECK(snprintf(text,sizeof(text),"import {Path} from path\nexport fn denied(p:Path,b:Array<u8>,s:string){defer{%s(%s)}}\n",names[n],arguments[n])>0);
   host_file_prepare_rejected(context,text,true,XR_XIR_BAD_TYPE,"E0392:");
  }
 }
 /* This body survives cold checking, lowering and real C emission. Its cleanup
  * permission does not claim that an unimplemented Close has run successfully. */
 CHECK(host_named(module,"closeInDefer")<module->function_count&&host_named(module,"byRef")<module->function_count);
 static const char *const denied[]={
  "export fn noRead(p:Path)->i64{return __fileOpen(p)}\n",
  "import {Path} from path\nclass Wrong{raw:string\ninit(raw:string){this.raw=raw}}\nexport fn fake(p:Wrong)->i64{return __fileOpen(p)}\n",
  "import {Path} from path\nexport fn conflict(v:Slice<u8>,p:Path)->i64{const opened=__fileOpen(p);return len(v)}\n"};
 for(unsigned n=0;n<3;++n)host_file_prepare_rejected(context,denied[n],true,n==2?XR_XIR_UNRESOLVED:XR_XIR_BAD_TYPE,n==2?"(CALL)":NULL);
}
#endif // HOST_OS_IO_FILE_PREPARE_CONTRACTS_H
