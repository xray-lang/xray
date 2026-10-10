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
#include "xir_construction_fixture.h"
#include "xir/xxir_constraint_proof.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_operand_roles.h"
static XrXirStatus unit_slot_verify(const XrXirArtifact *artifact,const XrXirModule *module) {
 size_t live=effects_compile_live,bytes=effects_compile_bytes;
 XrXirStatus status=xr_xir_compile_verify_v2(xr_xir_compile_artifact_context(artifact),module,
  xr_xir_compile_artifact_construction(artifact),NULL);
 CHECK(effects_compile_live==live&&effects_compile_bytes==bytes);return status;
}
static bool unit_cell_contract(const XrXirArtifact *artifact,XrXirModule *module,uint32_t f,uint32_t i,
 unsigned counts[3]) {
 XrXirFunction *fn=(XrXirFunction *)&module->functions[f];
 XrXirInstruction *ops=(XrXirInstruction *)fn->instructions,saved=ops[i];
 if(saved.op!=XR_XIR_CELL_NEW&&saved.op!=XR_XIR_CELL_READ&&saved.op!=XR_XIR_CELL_WRITE)return false;
 XrXirType physical=saved.op==XR_XIR_CELL_NEW?saved.type:xr_xir_operand_type(fn,saved.args[0]);
 if(!xr_xir_cell_is_unit(module->types,physical))return false;
 unsigned kind=saved.op==XR_XIR_CELL_NEW?0u:saved.op==XR_XIR_CELL_READ?1u:2u;++counts[kind];
 CHECK(saved.type==(kind?XR_XIR_UNIT:physical)&&!saved.args[1]);
 if(!kind){
  CHECK(!saved.args[0]);ops[i].args[0]=1;CHECK(unit_slot_verify(artifact,module)==XR_XIR_BAD_STRUCTURE);
 }else{
  ops[i].type=XR_XIR_I64;CHECK(unit_slot_verify(artifact,module)==XR_XIR_BAD_TYPE);
 }
 ops[i]=saved;ops[i].args[1]=1;CHECK(unit_slot_verify(artifact,module)==XR_XIR_BAD_STRUCTURE);
 ops[i]=saved;return true;
}
static void unit_slot_contracts(const XrXirArtifact *artifact) {
 const XrXirModule *original=xr_xir_compile_artifact_module(artifact);XrXirModule module=*original;
 XrXirFunction *functions=malloc(original->function_count*sizeof(*functions));CHECK(functions);
 memcpy(functions,original->functions,original->function_count*sizeof(*functions));module.functions=functions;
 unsigned unit_init=0,unit_load=0,cell_init=0,cell_load=0,foreign=0,cell_ops[3]={0};
 for(uint32_t f=0;f<module.function_count;++f){
  XrXirFunction *fn=&functions[f];XrXirInstruction *ops=malloc(fn->instruction_count*sizeof(*ops));CHECK(ops);
  memcpy(ops,fn->instructions,fn->instruction_count*sizeof(*ops));fn->instructions=ops;
  for(uint32_t i=0;i<fn->instruction_count;++i){XrXirInstruction saved=ops[i];
   if(unit_cell_contract(artifact,&module,f,i,cell_ops))continue;
   if(saved.op!=XR_XIR_SLOT_INIT&&saved.op!=XR_XIR_SLOT_STORE&&saved.op!=XR_XIR_SLOT_LOAD)continue;
   CHECK(saved.immediate>=0&&(uint64_t)saved.immediate<module.declarations->slot_count);
   const XrXirSlot *slot=&module.declarations->slots[saved.immediate];
   bool cell=xr_xir_cell_is_unit(module.types,slot->type);if(slot->type!=XR_XIR_UNIT&&!cell)continue;
   CHECK(slot->mutable==(uint32_t)cell&&saved.op!=XR_XIR_SLOT_STORE&&!saved.args[1]);
   if(saved.op==XR_XIR_SLOT_LOAD){
    CHECK(!saved.args[0]);if(cell)++cell_load;else ++unit_load;
    ops[i].type=XR_XIR_I64;CHECK(unit_slot_verify(artifact,&module)==XR_XIR_BAD_TYPE);
   }else{
    if(cell){
     ++cell_init;CHECK(xr_xir_operand_type(fn,saved.args[0])==slot->type);
     ops[i].args[0]=fn->parameter_count+fn->instruction_count;
     CHECK(unit_slot_verify(artifact,&module)==XR_XIR_BAD_VALUE);
    }else{
     ++unit_init;CHECK(!saved.args[0]);ops[i].args[0]=1;
     CHECK(unit_slot_verify(artifact,&module)==XR_XIR_BAD_STRUCTURE);
    }
    ops[i]=saved;ops[i].args[1]=1;CHECK(unit_slot_verify(artifact,&module)==XR_XIR_BAD_STRUCTURE);
    ops[i]=saved;ops[i].op=XR_XIR_SLOT_STORE;CHECK(unit_slot_verify(artifact,&module)==XR_XIR_BAD_STRUCTURE);
   }
   ops[i]=saved;ops[i].immediate=module.declarations->slot_count;
   CHECK(unit_slot_verify(artifact,&module)==XR_XIR_BAD_STRUCTURE);ops[i]=saved;
   for(uint32_t other=0;other<module.declarations->slot_count;++other)
    if(module.declarations->slots[other].module!=module.declarations->functions[f].module){
     ++foreign;ops[i].immediate=other;CHECK(unit_slot_verify(artifact,&module)==XR_XIR_BAD_STRUCTURE);ops[i]=saved;break;
    }
  }
  fn->instructions=original->functions[f].instructions;free(ops);
 }
 CHECK(unit_init&&unit_load&&cell_init&&cell_load&&unit_init+cell_init>=2&&foreign>=2);
 CHECK(cell_ops[0]&&cell_ops[1]&&cell_ops[2]);CHECK(unit_slot_verify(artifact,&module)==XR_XIR_OK);free(functions);
 printf("Unit const slots init/load=%u/%u; Cell<Unit> slots init/load=%u/%u new/read/write=%u/%u/%u foreign=%u\n",
  unit_init,unit_load,cell_init,cell_load,cell_ops[0],cell_ops[1],cell_ops[2],foreign);
 XrXirValue unit={0},copy={0};CHECK(xr_xir_value_valid(&unit));CHECK(xr_xir_value_copy(&unit,&copy)==XR_XIR_VALUE_OK);
 CHECK(!xr_xir_value_argument(&unit,NULL,XR_XIR_UNIT));xr_xir_value_drop(&copy);
 XrXirProofContext closed={original,{XR_XIR_CONTEXT_CLOSED,0,0}};const XrXirCompileContext *compile=xr_xir_compile_artifact_context(artifact);
 CHECK(xr_xir_compile_type_markers_prove(compile,&closed,XR_XIR_UNIT,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
 CHECK(xr_xir_compile_type_markers_prove(compile,&closed,XR_XIR_UNIT,XR_XIR_CONSTRAINT_ERROR)==XR_XIR_BAD_TYPE);
 puts("Unit and Cell slot exact arity, owner index, const, result type and independent marker contracts PASS");
}
#endif
