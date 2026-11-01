/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_stringable_source.c - Governed scalar witnesses after producer death
 *
 * KEY CONCEPT:
 *   A constrained body reaches actual helper code after cold witness resolution.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_implementation_verify.h"
#include "xir/xxir_constraint_proof_internal.h"
#include "xir/xxir_types.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_prelude_source.h"
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "base/xmalloc.h"
#include "base/xfileio.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"Stringable %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "stringable_runtime.h"
static XrXirStatus text_check(const XrXirCompileContext *context,const char *root,
    const char *text,XrXirSourceResult *output) {
 char path[2048],stdlib[2048];CHECK(snprintf(path,sizeof(path),"%s/tests/unit/xir/fixtures/stringable.xr",root)>0);
 CHECK(snprintf(stdlib,sizeof(stdlib),"%s/stdlib",root)>0);
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"stringable",root};
 XrXirSourceRequest request={.session=session,.entry_path=path,.authority=&authority,.context=context,.stdlib_path=stdlib,.linkage_kind=XR_XIR_PROGRAM};
 XrXirSourceText input={"tests/unit/xir/fixtures/stringable.xr",text,strlen(text)};
 XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
 XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,&failure);
 if(status!=XR_XIR_OK)fprintf(stderr,"Control%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
 xr_xir_compile_source_failure_free(&failure);xr_compile_session_free(session);return status;
}
static uint32_t stringable_interface(const XrXirModule *module) {
 const XrXirInterfaceTable *table=module->types->interfaces;
 CHECK(table && table->declarations);
 for(uint32_t i=0;i<table->count;++i)if(table->declarations[i].name.length==10 &&
  !memcmp(table->declarations[i].name.bytes,"Stringable",10))return i;
 CHECK(false);return 0;
}
typedef struct CancelState {uint32_t polls,stop;} CancelState;
static bool cancel_after(const void *opaque) {
 CancelState *state=(CancelState *)opaque;return ++state->polls>=state->stop;
}
static void witness_controls(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
 const XrXirModule *module=xr_xir_compile_artifact_module(artifact);
 uint32_t iface=stringable_interface(module);
 XrXirProofContext proof={module,{XR_XIR_CONTEXT_CLOSED,0,0}};
 XrXirWitnessRequest request={module,XR_XIR_I64,{iface,NULL,0},0};XrXirWitness result={0};
 CHECK(xr_xir_compile_witness_resolve(context,&proof,&request,&result)==XR_XIR_OK);
 uint32_t helper=result.function;CHECK(helper<module->function_count);
 XrXirFunction *functions=xr_malloc((size_t)module->function_count*sizeof(*functions));CHECK(functions);
 memcpy(functions,module->functions,(size_t)module->function_count*sizeof(*functions));
 XrXirModule forged=*module;forged.functions=functions;request.declaration_module=&forged;
 const XrXirInstruction constant[]={
  {XR_XIR_CONST_INT,XR_XIR_I64,{0,0},{0,0},42,{0}},
  {XR_XIR_TO_STRING,XR_XIR_STRING,{1,0},{0,0},0,{0}},
  {XR_XIR_RETURN,XR_XIR_UNIT,{2,0},{0,0},0,{0}}};
 const XrXirBlock block={0,3,0,0};functions[helper].instructions=constant;
 functions[helper].instruction_count=3;functions[helper].blocks=&block;
 result.function=UINT32_MAX;
 CHECK(xr_xir_compile_witness_resolve(context,&proof,&request,&result)==XR_XIR_BAD_TYPE && result.function==UINT32_MAX);
 forged.stage=XR_XIR_BUILT;forged.provenance=NULL;XrXirArtifact *changed=NULL,*closed=NULL;
 XrXirDiagnostic issue={0};XrXirStatus status=xr_xir_compile_check_v2(context,&forged,
  xr_xir_compile_artifact_construction(artifact),&changed,&issue);
 fprintf(stderr,"Changed helper check%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
 CHECK(xr_xir_compile_specialize(changed,&closed,&issue,NULL)==XR_XIR_BAD_TYPE && !closed);
 xr_xir_compile_artifact_free(changed);
 functions[helper]=module->functions[helper];functions[helper].result=XR_XIR_I64;
 CHECK(xr_xir_compile_witness_resolve(context,&proof,&request,&result)==XR_XIR_BAD_TYPE && result.function==UINT32_MAX);
 xr_free(functions);request.declaration_module=module;
 CancelState state={0,2};XrCompileRequestControl control={0};
 XrCompileRequestOptions options={cancel_after,&state,XR_COMPILE_DEADLINE_NONE,0};
 CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
 CHECK(xir_compile_witness_resolve_controlled(context,&proof,&request,&result,&control)==XR_XIR_CANCELLED);
 CHECK(state.polls==2 && result.function==UINT32_MAX);
 control=(XrCompileRequestControl){0};options=(XrCompileRequestOptions){NULL,NULL,XR_COMPILE_DEADLINE_AFTER,0};
 CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
 CHECK(xir_compile_witness_resolve_controlled(context,&proof,&request,&result,&control)==XR_XIR_DEADLINE && result.function==UINT32_MAX);
 CHECK(xr_xir_compile_witness_resolve(context,&proof,&request,&result)==XR_XIR_OK);
}
static void source_controls(const XrXirCompileContext *context,const char *root) {
 static const char *const rejected[]={
  "fn text<T>(value:T)->string{return value.toString()}",
  "fn text<T:Stringable>(value:T)->string{return value.toString()};print(text<bool>(true))",
  "interface Stringable{toString()->string};fn text<T:Stringable>(value:T)->string{return value.toString()};print(text<i64>(42))",
  "fn text<T:Stringable>(value:T)->string{return value.toString()};print(text<Array<i64>>([42]))"};
 XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
 for(size_t i=0;i<sizeof(rejected)/sizeof(*rejected);++i) {
  XrXirSourceResult output={0};CHECK(text_check(context,root,rejected[i],&output)!=XR_XIR_OK && !output.checked);
  CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
 }
 char source[8192];size_t used=(size_t)snprintf(source,sizeof(source),
  "fn text<T:Stringable>(value:T)->string{return value.toString()};print(text<i64>(42));\n");
 for(uint32_t i=0;i<80;++i){int n=snprintf(source+used,sizeof(source)-used,"fn unrelated%u()->i64{return 1}\n",i);CHECK(n>0 && (size_t)n<sizeof(source)-used);used+=(size_t)n;}
 XrXirSourceResult output={0};CHECK(text_check(context,root,source,&output)==XR_XIR_OK);
 witness_controls(context,output.checked);xr_xir_compile_source_result_free(&output);
 CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==before.held_bytes);
}
static XrXirLibraryCatalog *prelude_catalog(const XrXirCompileContext *context,const char *root) {
 XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
 XrXirStatus status=xr_xir_compile_prelude_library(context,&source,&diagnostic);
 fprintf(stderr,"Prelude%u %s\n",status,diagnostic.message);CHECK(status==XR_XIR_OK);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
 xr_xir_compile_source_result_free(&source);
 char physical[2048];CHECK(snprintf(physical,sizeof(physical),"%s/stdlib",root)>0);
 XrOsIoPolicy policy=xr_compile_io_policy(context->resources);char *canonical_root=NULL;
 CHECK(xr_realpath_owned(&policy,physical,&canonical_root)==XR_OS_IO_OK);
 XrXirLibraryModuleInput binding={{XR_MODULE_IDENTITY_STDLIB,"prelude",canonical_root},"prelude/builtin_symbols.def"};
 XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=&binding,.module_count=1};
 xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
 status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);fprintf(stderr,"Catalog%u\n",status);
 xr_compile_resources_free(canonical_root);
 CHECK(status==XR_XIR_OK && catalog);memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
 return catalog;
}
int main(int argc,char **argv) {
 CHECK(argc==2 || argc==3);XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};XrXirCompileContext context={0};
 CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
 XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
 XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILE_RESOURCE_OK);
 char entry[2048],stdlib[2048];CHECK(snprintf(entry,sizeof(entry),"%s/tests/unit/xir/fixtures/stringable.xr",argv[1])>0);
 CHECK(snprintf(stdlib,sizeof(stdlib),"%s/stdlib",argv[1])>0);
 XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_PROJECT,"stringable",argv[1]};
 XrXirSourceRequest request={.session=session,.entry_path=entry,.authority=&authority,.context=&context,.stdlib_path=stdlib,.linkage_kind=XR_XIR_PROGRAM};
 XrXirLibraryCatalog *catalog=prelude_catalog(&context,argv[1]);request.libraries=catalog;
 XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
 XrXirStatus status=xr_xir_compile_source_check(&request,&source,&diagnostic,&failure);
 fprintf(stderr,"Source%u %s:%d:%d %s\n",status,failure.path?failure.path:"",diagnostic.line,diagnostic.column,diagnostic.message);
 CHECK(status==XR_XIR_OK);xr_xir_compile_source_failure_free(&failure);xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
 XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
 xr_xir_compile_source_result_free(&source);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
 CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
 memset(packet.bytes,0x5a,packet.length);xr_xir_compile_checked_packet_free(&packet);
 XrXirDiagnostic issue={0};status=xr_xir_compile_specialize(checked,&closed,&issue,NULL);
 fprintf(stderr,"Specialize%u f%u b%u i%u\n",status,issue.function,issue.block,issue.instruction);CHECK(status==XR_XIR_OK);
 xr_xir_compile_artifact_free(checked);const XrXirTarget target={XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION};
 CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,&issue)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
 uint32_t selected=xr_xir_compile_artifact_module(lowered)->declarations->entry_function;
 if(argc==2) source_controls(&context,argv[1]);
 if(argc==3) {
  XrXirCSource code={0};CHECK(xr_xir_compile_emit_c(lowered,"stringable",8388608,&code)==XR_XIR_OK);
  FILE *file=fopen(argv[2],"wb");CHECK(file && fwrite(code.text,1,code.length,file)==code.length);
  CHECK(fprintf(file,"\nconst uint32_t stringable_entry=%uu;\n",selected)>0 && !fclose(file));
  xr_xir_compile_c_source_free(&code);xr_xir_compile_artifact_free(lowered);
 } else {
  XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
  stringable_run(program,selected,"Box(42)\nBox(hello)\n(age, 25)\n");
 }
 CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
 xr_compile_resources_release(context.resources);puts("Stringable Source cold and scalar witness PASS");return 0;
}
