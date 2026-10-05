/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_unit_slot_contract_cases.h - Exact empty slot instruction contracts
 *
 * KEY CONCEPT:
 *   No payload is distinct from an operand encoding or an unpublished slot.
 */
#ifndef XIR_UNIT_SLOT_CONTRACT_CASES_H
#define XIR_UNIT_SLOT_CONTRACT_CASES_H
#include "xir/xxir_constraint_proof.h"
static void unit_slot_contracts(const XrXirArtifact *artifact) {
 const XrXirModule *original=xr_xir_compile_artifact_module(artifact);XrXirModule module=*original;
 XrXirFunction *functions=malloc(original->function_count*sizeof(*functions));CHECK(functions);
 memcpy(functions,original->functions,original->function_count*sizeof(*functions));module.functions=functions;
 unsigned unit_init=0,unit_store=0,unit_load=0;
 for(uint32_t f=0;f<module.function_count;++f){
  XrXirFunction *fn=&functions[f];XrXirInstruction *ops=malloc(fn->instruction_count*sizeof(*ops));CHECK(ops);
  memcpy(ops,fn->instructions,fn->instruction_count*sizeof(*ops));fn->instructions=ops;
  for(uint32_t i=0;i<fn->instruction_count;++i){XrXirInstruction saved=ops[i];
   if(saved.op!=XR_XIR_SLOT_INIT&&saved.op!=XR_XIR_SLOT_STORE&&saved.op!=XR_XIR_SLOT_LOAD)continue;
   const XrXirSlot *slot=&module.declarations->slots[saved.immediate];if(slot->type!=XR_XIR_UNIT)continue;
   CHECK(!saved.args[0]&&!saved.args[1]);
   if(saved.op==XR_XIR_SLOT_LOAD){++unit_load;ops[i].type=XR_XIR_I64;CHECK(xr_xir_compile_verify(xr_xir_compile_artifact_context(artifact),&module,NULL)==XR_XIR_BAD_TYPE);}
   else{if(saved.op==XR_XIR_SLOT_INIT)++unit_init;else ++unit_store;
    ops[i].args[0]=1;CHECK(xr_xir_compile_verify(xr_xir_compile_artifact_context(artifact),&module,NULL)==XR_XIR_BAD_STRUCTURE);
    ops[i]=saved;ops[i].args[1]=1;CHECK(xr_xir_compile_verify(xr_xir_compile_artifact_context(artifact),&module,NULL)==XR_XIR_BAD_STRUCTURE);
    if(!slot->mutable){ops[i]=saved;ops[i].op=XR_XIR_SLOT_STORE;CHECK(xr_xir_compile_verify(xr_xir_compile_artifact_context(artifact),&module,NULL)==XR_XIR_BAD_STRUCTURE);}
   }
   ops[i]=saved;ops[i].immediate=module.declarations->slot_count;CHECK(xr_xir_compile_verify(xr_xir_compile_artifact_context(artifact),&module,NULL)==XR_XIR_BAD_STRUCTURE);
   ops[i]=saved;
   for(uint32_t other=0;other<module.declarations->slot_count;++other)
    if(module.declarations->slots[other].type==XR_XIR_UNIT&&module.declarations->slots[other].module!=module.declarations->functions[f].module){
     ops[i].immediate=other;CHECK(xr_xir_compile_verify(xr_xir_compile_artifact_context(artifact),&module,NULL)==XR_XIR_BAD_STRUCTURE);ops[i]=saved;break;
    }
  }
  fn->instructions=original->functions[f].instructions;free(ops);
 }
 CHECK(unit_init>=2&&unit_store&&unit_load);CHECK(xr_xir_compile_verify(xr_xir_compile_artifact_context(artifact),&module,NULL)==XR_XIR_OK);free(functions);
 XrXirValue unit={0},copy={0};CHECK(xr_xir_value_valid(&unit));CHECK(xr_xir_value_copy(&unit,&copy)==XR_XIR_VALUE_OK);
 CHECK(!xr_xir_value_argument(&unit,NULL,XR_XIR_UNIT));xr_xir_value_drop(&copy);
 XrXirProofContext closed={original,{XR_XIR_CONTEXT_CLOSED,0,0}};const XrXirCompileContext *compile=xr_xir_compile_artifact_context(artifact);
 CHECK(xr_xir_compile_type_markers_prove(compile,&closed,XR_XIR_UNIT,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
 CHECK(xr_xir_compile_type_markers_prove(compile,&closed,XR_XIR_UNIT,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
 puts("Unit slot exact arity, owner index, const, result type and independent marker contracts PASS");
}
#endif
