/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_stat_contracts.h - Exact native identity and denied language authority
 */
#ifndef HOST_OS_STAT_CONTRACTS_H
#define HOST_OS_STAT_CONTRACTS_H
static void host_stat_contracts(XrXirCompileContext *context,const XrXirArtifact *code){
 const XrXirModule *module=xr_xir_compile_artifact_module(code);uint32_t f=host_named(module,"__stat");
 CHECK(module->declarations->functions[f].native_callable==XR_NATIVE_CALLABLE_IO_STAT&&xir_native_callable_verify(module,f)==XR_XIR_OK);
 const XrXirTypeNode *optional=xr_xir_type_node(module->types,module->functions[f].result);CHECK(optional);
 XrXirType type=optional->element;CHECK(xir_file_stat_nullable(module->types,module->functions[f].result));
 const XrXirTypeNode *node=xr_xir_type_node(module->types,type);CHECK(node);uint32_t d=node->nominal.declaration;
 const XrXirNominalTable *table=module->types->nominals;XrXirNominalIdentity identity={0};XrXirNominalFieldIdentity fields[10];
 if(table->declarations){const XrXirNominalDeclaration *decl=table->declarations+d;
  identity=(XrXirNominalIdentity){decl->module,decl->name,decl->exported,decl->parameter_count,fields,decl->field_count,decl->kind,decl->variants,decl->variant_count,decl->flags,decl->native,decl->parent};
  for(uint32_t n=0;n<10;++n)fields[n]=(XrXirNominalFieldIdentity){decl->fields[n].name,decl->fields[n].flags};
 }else{identity=table->identities[d];memcpy(fields,identity.fields,sizeof(fields));identity.fields=fields;}
 CHECK(xir_file_stat_header(&identity));
 XrXirProgramPacket packet=xir_program_packet_borrow(code);CHECK(packet.bytes&&packet.length>32);
 unsigned char *bytes=xr_malloc(packet.length);CHECK(bytes);memcpy(bytes,packet.bytes,packet.length);size_t matches=0;
 for(size_t at=0;at+32<=packet.length;++at)if(!memcmp(bytes+at,identity.native.source_fingerprint,32)){
  bytes[at]^=1;++matches;at+=31;
 }
 CHECK(matches==1);XrXirArtifact *denied=NULL;
 CHECK(xr_xir_compile_checked_read_lower(context,bytes,packet.length,&code->target,&denied,NULL)==XR_XIR_BAD_STRUCTURE&&!denied);xr_free(bytes);
 for(unsigned n=0;n<8;++n){XrXirNominalIdentity bad=identity;
  switch(n){case 0:bad.native.native_id=0;break;case 1:bad.arity=1;break;case 2:bad.parent=d+1;break;
   case 3:bad.flags=0;break;case 4:bad.exported=1;break;case 5:bad.native.source_fingerprint[0]^=1;break;
   case 6:bad.kind=XR_XIR_NOMINAL_STRUCT;break;default:bad.module=(XrXirLiteral){"io",2};break;}
  CHECK(!xir_file_stat_header(&bad));
 }
 XrXirNominalFieldIdentity saved=fields[0];fields[0]=fields[1];CHECK(!xir_file_stat_header(&identity));fields[0]=saved;
 fields[0].flags=XR_XIR_FIELD_PRIVATE;CHECK(!xir_file_stat_header(&identity));fields[0].flags=XR_XIR_FIELD_MUTABLE;
 CHECK(!xir_file_stat_header(&identity));fields[0]=saved;
 for(uint32_t n=0;n<10;++n)CHECK(!xir_file_stat_field_type(module->types,n,n<5?XR_XIR_BOOL:XR_XIR_I64));
 CHECK(xr_xir_compile_nominal_access(context,module,f,type,0,XR_XIR_NOMINAL_READ)==XR_XIR_OK);
 CHECK(xr_xir_compile_nominal_access(context,module,f,type,0,XR_XIR_NOMINAL_CONSTRUCT)==XR_XIR_BAD_TYPE);
 CHECK(xr_xir_compile_nominal_access(context,module,f,type,0,XR_XIR_NOMINAL_WRITE)==XR_XIR_BAD_TYPE);
 XrXirModule changed=*module;XrXirDeclarations scope=*module->declarations;XrXirFunctionIdentity methods[64];CHECK(module->function_count<64);
 memcpy(methods,scope.functions,module->function_count*sizeof(*methods));scope.functions=methods;changed.declarations=&scope;
 for(uint32_t kind=XR_XIR_READ_METHOD;kind<=XR_XIR_CONSTRUCTOR_FIELDS;++kind){
  methods[f]=module->declarations->functions[f];methods[f].native_callable=0;methods[f].nominal_owner=d+1;methods[f].method_kind=kind;
  CHECK(xr_xir_compile_method_signature_verify(context,&changed,f)==XR_XIR_BAD_TYPE);
 }
 XrXirSourceModule modules[16];CHECK(scope.module_count<16);memcpy(modules,scope.modules,scope.module_count*sizeof(*modules));scope.modules=modules;
 methods[f]=module->declarations->functions[f];uint32_t owner=methods[f].module;
 modules[owner].dependencies=NULL;modules[owner].dependency_count=0;
 CHECK(xir_native_callable_verify(&changed,f)==XR_XIR_BAD_TYPE);
 static const char *const rejected[]={
  "import {Path} from path\nexport fn bad(){var value=__FileStat()}\n",
  "import {Path} from path\nclass __FileStat{size:i64}\n",
  "import {Path} from path\nexport fn bad(p:Path){var value=__stat(p);if(value!=null){value!.size=7}}\n",
  "import {Path} from path\nclass Spoof{size:i64\ninit(){this.size=0}}\nexport fn bad(p:Path)->bool{return __stat(Spoof())!=null}\n"
 };
 for(unsigned n=0;n<sizeof(rejected)/sizeof(*rejected);++n){
  XrXirSourceResult output={0};XrXirSourceDiagnostic diagnostic={0};
  XrXirStatus status=host_check(context,"io",rejected[n],true,&output,&diagnostic);
  fprintf(stderr,"stat source reject%u status%u %s\n",n,status,diagnostic.message);
  CHECK(status==XR_XIR_BAD_TYPE&&!output.checked&&!output.snapshot);xr_xir_compile_source_result_free(&output);
 }
 host_ctfe(code,host_named(module,"ctfeStat"),NULL,0);
}
#endif // HOST_OS_STAT_CONTRACTS_H
