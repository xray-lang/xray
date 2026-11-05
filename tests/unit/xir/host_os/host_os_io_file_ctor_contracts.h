/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_ctor_contracts.h - Cold ordinary constructor access controls
 */
#ifndef HOST_OS_IO_FILE_CTOR_CONTRACTS_H
#define HOST_OS_IO_FILE_CTOR_CONTRACTS_H
#include "xir/xxir_view_prepare_internal.h"
#include "xir/xxir_internal.h"
#include "xir/xxir_native_callable.h"
#include "host_os_io_file_write_contracts.h"
static void host_file_ctor_boundary_controls(XrXirCompileContext *context,const XrXirArtifact *cold){
 const XrXirModule *module=xr_xir_compile_artifact_module(cold);
 const XrXirConstruction *construction=xr_xir_compile_artifact_construction(cold);
 const XrXirBorrowReturns *returns=xr_xir_compile_construction_borrow_returns(construction);CHECK(returns);
 uint32_t entry=host_named(module,"fileBuffered"),constructor=UINT32_MAX,borrow_site=UINT32_MAX;
 const XrXirFunction *body=module->functions+entry;
 for(uint32_t i=0;i<body->instruction_count;++i){const XrXirInstruction *op=body->instructions+i;
  if(op->op!=XR_XIR_CALL||op->immediate<0||(uint64_t)op->immediate>=module->function_count||op->args[1]!=1)continue;
  bool receiver=false;
  for(uint32_t r=0;r<returns->count;++r)if(returns->rows[r].function==(uint32_t)op->immediate&&
   returns->rows[r].native_callable==XR_NATIVE_CALLABLE_BUFFER_AS_BYTES)receiver=true;
  if(!receiver)continue;
  CHECK(op->args[0]<body->operand_count);uint32_t value=body->operands[op->args[0]];
  CHECK(value>=body->parameter_count&&value-body->parameter_count<i);
  const XrXirInstruction *producer=body->instructions+value-body->parameter_count;
  CHECK(producer->op==XR_XIR_CALL&&producer->immediate>=0&&(uint64_t)producer->immediate<module->function_count);
  CHECK(constructor==UINT32_MAX);constructor=(uint32_t)producer->immediate;borrow_site=i;
 }
 CHECK(constructor!=UINT32_MAX&&borrow_site!=UINT32_MAX&&returns);
 CHECK(module->declarations->functions[constructor].method_kind==XR_XIR_CONSTRUCTOR&&
  module->declarations->functions[entry].module!=module->declarations->functions[constructor].module);
 CHECK(sizeof(XrXirFunctionIdentity)<=SIZE_MAX/module->function_count);
 XrXirFunctionIdentity *identities=xr_malloc(module->function_count*sizeof(*identities));CHECK(identities);
 memcpy(identities,module->declarations->functions,module->function_count*sizeof(*identities));
 XrXirDeclarations declarations=*module->declarations;declarations.functions=identities;
 XrXirModule raw=*module;raw.stage=XR_XIR_BUILT;raw.provenance=NULL;raw.declarations=&declarations;
 XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
 XrXirDiagnostic diagnostic={0};XirEffectInput input={&raw,construction,NULL};
 CHECK(xir_compile_view_prepare_ordinary(context,input,&diagnostic)==XR_XIR_OK);
 CHECK(xr_xir_compile_structure_verify_v2(context,&raw,construction,&diagnostic)==XR_XIR_OK);
 host_file_write_boundary_controls(context,&raw,construction,entry,borrow_site);
 /* A legal static factory keeps the actual Class body and complete signature.
  * Its declaration alone must not gain the constructor boundary. */
 identities[constructor].method_kind=XR_XIR_STATIC_METHOD;
 CHECK(xir_compile_view_prepare_ordinary(context,input,&diagnostic)==XR_XIR_OK);
 XrXirStatus status=xr_xir_compile_structure_verify_v2(context,&raw,construction,&diagnostic);
 if(status!=XR_XIR_UNRESOLVED||diagnostic.function!=entry||diagnostic.instruction!=borrow_site)
  fprintf(stderr,"cold constructor factory status%u f%u b%u i%u expected f%u i%u\n",status,
   diagnostic.function,diagnostic.block,diagnostic.instruction,entry,borrow_site);
 CHECK(status==XR_XIR_UNRESOLVED&&diagnostic.function==entry&&diagnostic.instruction==borrow_site);
 identities[constructor]=module->declarations->functions[constructor];
 identities[constructor].member_access=XR_XIR_MEMBER_PRIVATE;
 CHECK(xir_compile_view_prepare_ordinary(context,input,&diagnostic)==XR_XIR_BAD_STRUCTURE);
 identities[constructor]=module->declarations->functions[constructor];
 CHECK(xr_xir_compile_structure_verify_v2(context,&raw,construction,&diagnostic)==XR_XIR_OK);
 xr_free(identities);CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
}
#endif // HOST_OS_IO_FILE_CTOR_CONTRACTS_H
