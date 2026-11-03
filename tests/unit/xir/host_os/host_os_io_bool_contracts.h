/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_bool_contracts.h - Every filesystem path needs its own authority
 */
#ifndef HOST_OS_IO_BOOL_CONTRACTS_H
#define HOST_OS_IO_BOOL_CONTRACTS_H
static void host_io_bool_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const XrXirModule *module=xr_xir_compile_artifact_module(code);
 const char *names[]={"__mkdir","__rmdir","__remove","__rename","__utimeNow"};
 for(unsigned n=0;n<5;++n){uint32_t f=host_named(module,names[n]);
  CHECK(module->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_IO_MKDIR+n&&xir_native_callable_verify(module,f)==XR_XIR_OK);
 }
 uint32_t rename=host_named(module,"__rename");XrXirModule changed=*module;XrXirFunction functions[64];CHECK(module->function_count<64);
 memcpy(functions,module->functions,module->function_count*sizeof(*functions));changed.functions=functions;
 XrXirType parameters[2]={functions[rename].parameters[0],functions[rename].parameters[1]};functions[rename].parameters=parameters;
 for(unsigned p=0;p<2;++p){XrXirType saved=parameters[p];parameters[p]=XR_XIR_STRING;
  CHECK(xir_native_callable_verify(&changed,rename)==XR_XIR_BAD_TYPE);parameters[p]=saved;
 }
 XrXirDeclarations scope=*module->declarations;XrXirSourceModule modules[16];CHECK(scope.module_count<16);
 memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));scope.modules=modules;changed.declarations=&scope;
 uint32_t owner=scope.functions[rename].module;modules[owner].dependencies=NULL;modules[owner].dependency_count=0;
 CHECK(xir_native_callable_verify(&changed,rename)==XR_XIR_BAD_TYPE);
 static const char *const rejected[]={
  "import {Path} from path\nexport fn wrong(p:Path)->bool{return __rename(42,p)}\n",
  "import {Path} from path\nexport fn wrong(p:Path)->bool{return __rename(p,42)}\n",
  "export fn noRead(p:Path)->bool{return __rename(p,p)}\n",
  "import {Path} from path\nexport fn conflict(v:Slice<u8>,a:Path,b:Path)->i64{var changed=__rename(a,b);return len(v)}\n"
 };
 for(unsigned n=0;n<sizeof(rejected)/sizeof(*rejected);++n){XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=host_check(context,"io",rejected[n],true,&output,&diagnostic);
  CHECK(status==(n==3?XR_XIR_UNRESOLVED:XR_XIR_BAD_TYPE)&&!output.checked&&!output.snapshot);
  if(n==3)CHECK(strstr(diagnostic.message,"view lifetime normalization")&&strstr(diagnostic.message,"(CALL)"));
  xr_xir_compile_source_result_free(&output);
 }
 const char *alias="import {Path} from path\nfn pair(a:ref Path,b:ref Path)->bool{return __rename(a,b)}\nexport fn bad()->bool{var p=Path(\".\");return pair(ref p,ref p)}\n";
 XrXirSourceResult denied={0};XrXirSourceDiagnostic diagnostic={0};
 CHECK(host_check(context,"io",alias,true,&denied,&diagnostic)==XR_XIR_BAD_TYPE&&!denied.checked&&!denied.snapshot&&strstr(diagnostic.message,"must not alias"));
 xr_xir_compile_source_result_free(&denied);
 host_ctfe(code,host_named(module,"ctfeMutation"),NULL,0);
}
#endif // HOST_OS_IO_BOOL_CONTRACTS_H
