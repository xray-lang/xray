/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_contracts.h - Exact file authority and effect replay
 *
 * KEY CONCEPT:
 *   Declaration authority and cleanup transport require independent proof.
 *   Every other file primitive retains its real suspension obligation.
 */
#ifndef HOST_OS_IO_FILE_CONTRACTS_H
#define HOST_OS_IO_FILE_CONTRACTS_H
static const char host_file_contract_text[]=
 "import {Path} from path\n"
 "class Child extends Path{tail:string\ninit(raw:string){super(raw);this.tail=\"file-prepare-tail\"}}\n"
 "export fn make(raw:string)->Path{return Child(raw)}\n"
 "export fn closeFile(handle:i64)->bool{return __fileClose(handle)}\n"
 "export fn flushFile(handle:i64)->bool{return __fileFlush(handle)}\n"
 "export fn openFile(path:Path)->i64{return __fileOpen(path)}\n"
 "export fn openWrite(path:Path,append:bool)->i64{return __fileOpenWrite(path,append)}\n"
 "export fn readFile(handle:i64,maxBytes:i64)->Array<u8>?{return __fileRead(handle,maxBytes)}\n"
 "export fn writeFile(handle:i64,data:Array<u8>,offset:i64)->i64{return __fileWrite(handle,data,offset)}\n"
 "export fn writeText(handle:i64,data:string,offset:i64)->i64{return __fileWriteStr(handle,data,offset)}\n"
 "fn refOpen(path:ref Path)->i64{return __fileOpen(path)}\n"
 "export fn byRef(raw:string)->i64{var path:Path=Child(raw);return refOpen(ref path)}\n"
 "export fn closeInDefer(handle:i64){defer{__fileClose(handle)}}\n"
 "export fn ctfeClose()->bool{return __fileClose(3)}\n"
 "export fn ctfeFlush()->bool{return __fileFlush(1)}\n"
 "export fn ctfeOpen()->i64{return __fileOpen(Child(\".\"))}\n"
 "export fn ctfeOpenWrite()->i64{return __fileOpenWrite(Child(\".\"),false)}\n"
 "export fn ctfeRead()->Array<u8>?{return __fileRead(0,7)}\n"
 "export fn ctfeWrite()->i64{const data:Array<u8>=[0 as u8,255 as u8];return __fileWrite(1,data,0)}\n"
 "export fn ctfeWriteStr()->i64{return __fileWriteStr(2,\"owned\",0)}\n";
static void host_file_contract_raw_references(XrXirCompileContext *context){
 static const char text[]="import {Path} from path\nfn plainHandle(handle:i64)->bool{return handle>0}\n"
  "export fn reference()->fn(i64)->bool{return plainHandle}\n";
 XrXirArtifact *code=host_code(context,"io",text);const XrXirModule *module=xr_xir_compile_artifact_module(code);
 uint32_t function=host_named(module,"reference"),instruction=UINT32_MAX;
 const XrXirFunction *body=module->functions+function;CHECK(body->instructions&&body->instruction_count);
 for(uint32_t i=0;i<body->instruction_count;++i)if(body->instructions[i].op==XR_XIR_FUNCTION_REF){CHECK(instruction==UINT32_MAX);instruction=i;}
 CHECK(instruction!=UINT32_MAX);const XrXirInstruction *original=body->instructions+instruction;
 const XrXirTypeNode *signature=xr_xir_callable_signature(module->types,original->type);
 CHECK(signature&&signature->parameter_count==1&&signature->parameters&&signature->parameters[0].type==XR_XIR_I64&&
  signature->parameters[0].mode==XR_PARAM_READ&&signature->result==XR_XIR_BOOL&&signature->element==XR_XIR_NEVER&&
  !(signature->flags&XR_XIR_CALLABLE_NO_SUSPEND)&&!original->args[0]&&!original->args[1]);
 XrXirFunction *functions=xr_malloc(module->function_count*sizeof(*functions));
 XrXirInstruction *instructions=xr_malloc(body->instruction_count*sizeof(*instructions));CHECK(functions&&instructions);
 memcpy(functions,module->functions,module->function_count*sizeof(*functions));memcpy(instructions,body->instructions,body->instruction_count*sizeof(*instructions));
 XrXirModule changed=*module;changed.functions=functions;functions[function].instructions=instructions;XrXirDiagnostic diagnostic={0};
 CHECK(xr_xir_compile_structure_verify_v2(context,&changed,code->construction,&diagnostic)==XR_XIR_OK);
 const char *targets[]={"__fileClose","__fileFlush"};
 for(unsigned n=0;n<2;++n){
  uint32_t target=host_named(module,targets[n]);const XrXirFunction *callee=module->functions+target;
  CHECK(callee->parameter_count==1&&callee->parameters[0]==XR_XIR_I64&&callee->result==XR_XIR_BOOL&&callee->error_type==XR_XIR_NEVER);
  instructions[instruction].immediate=target;diagnostic=(XrXirDiagnostic){0};
  CHECK(xr_xir_compile_structure_verify_v2(context,&changed,code->construction,&diagnostic)==XR_XIR_BAD_TYPE&&
   diagnostic.function==function&&diagnostic.instruction==instruction);
 }
 instructions[instruction]=*original;CHECK(xr_xir_compile_structure_verify_v2(context,&changed,code->construction,NULL)==XR_XIR_OK);
 xr_free(instructions);xr_free(functions);xr_xir_compile_artifact_free(code);
}
static void host_file_contract_rejected(XrXirCompileContext *context,const char *text,bool genuine,
 XrXirStatus expected,const char *reason){
 XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=host_check(context,"io",text,genuine,&output,&diagnostic);
 if(status!=expected)fprintf(stderr,"file contract status%u expected%u %d:%d %s\n",status,expected,diagnostic.line,diagnostic.column,diagnostic.message);
 CHECK(status==expected&&!output.checked&&!output.snapshot&&diagnostic.status==expected);
 if(reason)CHECK(strstr(diagnostic.message,reason));
 xr_xir_compile_source_result_free(&output);
}
static void host_file_refusal_signature_positives(XrXirCompileContext *context){
 static const char *const admitted[]={
  "final class C{data:Array<u8> init(){this.data=[]} append(input:Slice<u8>)->i64{return len(input)}}\n",
  "final class C<T>{data:Array<u8> init(){this.data=[]} append(input:Slice<u8>){}}\n",
  "final class C{data:Array<Array<u8>> init(){this.data=[[0 as u8]]} append(input:Slice<u8>){}}\n",
  "final class C{data:Array<u8> init(){this.data=[]}}\nfn append<T>(value:C,input:Slice<u8>){}\n",
  "final class C{data:Array<i64> init(){this.data=[1]} append(input:Slice<u8>)->i64{return input[0] as i64}}\n"};
 for(unsigned n=0;n<sizeof(admitted)/sizeof(*admitted);++n){
  XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=host_check(context,"io",admitted[n],false,&source,&diagnostic);
  if(status!=XR_XIR_OK)fprintf(stderr,"view refusal signature positive%u status%u %d:%d %s\n",
   n,status,diagnostic.line,diagnostic.column,diagnostic.message);
  CHECK(status==XR_XIR_OK&&source.checked);xr_xir_compile_source_result_free(&source);
 }
}
static void host_file_byte_slice_refusals(XrXirCompileContext *context){
 host_file_refusal_signature_positives(context);
 static const char *const rejected[]={
  "final class C{data:Array<i64> init(){this.data=[1]} append(input:Slice<i64>){this.data.push(input[0])}}\n",
  "final class C<T>{data:Array<u8> init(){this.data=[]} append(input:Slice<u8>){this.data.push(input[0])}}\n",
  "final class C{data:Array<u8> init(){this.data=[]} append(input:MutSlice<u8>){this.data.push(input[0])}}\n",
  "final class C{data:Array<Array<u8>> init(){this.data=[[0 as u8]]} append(input:Slice<u8>){this.data[0].push(input[0])}}\n",
  "final class C{data:Array<u8> init(){this.data=[]} append(input:(Slice<u8>,i64)){this.data.push(input.0[0])}}\n",
  "final class C{data:Array<u8> init(){this.data=[]}}\nfn append<T>(value:C,input:Slice<u8>){value.data.push(input[0])}\n"};
 static const XrXirStatus expected[]={XR_XIR_BAD_TYPE,XR_XIR_UNRESOLVED,XR_XIR_BAD_TYPE,
  XR_XIR_UNRESOLVED,XR_XIR_BAD_TYPE,XR_XIR_UNRESOLVED};
 _Static_assert(sizeof(expected)/sizeof(*expected)==sizeof(rejected)/sizeof(*rejected),"each original refusal keeps its own boundary");
 for(unsigned n=0;n<sizeof(rejected)/sizeof(*rejected);++n)
  host_file_contract_rejected(context,rejected[n],false,expected[n],
   expected[n]==XR_XIR_BAD_TYPE?"constructed XIR failed checking":"(ARRAY_PUSH)");
 /* Keep a genuine byte-Slice entry so the nonbyte field rejection reaches
  * the Array mutation boundary rather than an unsupported input signature. */
 host_file_contract_rejected(context,"final class C{data:Array<i64> init(){this.data=[1]} append(input:Slice<u8>)->i64{this.data.push(input[0] as i64);return 0}}\n",
  false,XR_XIR_UNRESOLVED,"(ARRAY_PUSH)");
 const char *projected="fn edit(value:ref Array<u8>){}\nfinal class C{data:Array<u8> init(){this.data=[];edit(ref this.data)}}\n";
 host_file_contract_rejected(context,projected,false,XR_XIR_BAD_TYPE,"ref requires a mutable stored binding");
}
static void host_file_contracts(XrXirCompileContext *context){
 host_file_byte_slice_refusals(context);
 XrXirArtifact *code=host_code(context,"io",host_file_contract_text);
 host_file_contract_raw_references(context);
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
  CHECK(xir_native_callable_verify(&changed,function)==XR_XIR_BAD_STRUCTURE);identities[function]=module->declarations->functions[function];
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
  host_file_contract_rejected(context,text,true,XR_XIR_BAD_TYPE,NULL);
  CHECK(snprintf(text,sizeof(text),"export fn denied(){const value=%s}\n",names[n])>0);
  host_file_contract_rejected(context,text,false,XR_XIR_BAD_VALUE,NULL);
  CHECK(snprintf(text,sizeof(text),"import {Path} from path\nexport fn denied(p:Path,b:Array<u8>,s:string)->%s{const f=%s;return f(%s)}\n",results[n],names[n],arguments[n])>0);
  host_file_contract_rejected(context,text,true,XR_XIR_BAD_TYPE,NULL);
  if(n){
   CHECK(snprintf(text,sizeof(text),"import {Path} from path\nexport fn denied(p:Path,b:Array<u8>,s:string){defer{%s(%s)}}\n",names[n],arguments[n])>0);
   host_file_contract_rejected(context,text,true,XR_XIR_BAD_TYPE,"E0392:");
  }
 }
 /* The complete File program separately executes successful cleanup. These
  * bodies retain the direct-call and ordinary ref argument authority checks. */
 CHECK(host_named(module,"closeInDefer")<module->function_count&&host_named(module,"byRef")<module->function_count);
 static const char *const denied[]={
  "export fn noRead(p:Path)->i64{return __fileOpen(p)}\n",
  "import {Path} from path\nclass Wrong{raw:string\ninit(raw:string){this.raw=raw}}\nexport fn fake(p:Wrong)->i64{return __fileOpen(p)}\n",
  "import {Path} from path\nexport fn conflict(v:Slice<u8>,p:Path)->i64{const opened=__fileOpen(p);return len(v)}\n"};
 for(unsigned n=0;n<3;++n)host_file_contract_rejected(context,denied[n],true,n==2?XR_XIR_UNRESOLVED:XR_XIR_BAD_TYPE,n==2?"(CALL)":NULL);
 xr_xir_compile_artifact_free(code);
}
#endif // HOST_OS_IO_FILE_CONTRACTS_H
