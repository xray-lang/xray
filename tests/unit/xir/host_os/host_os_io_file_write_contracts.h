/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_io_file_write_contracts.h - Cold complete write-footprint controls
 */
#ifndef HOST_OS_IO_FILE_WRITE_CONTRACTS_H
#define HOST_OS_IO_FILE_WRITE_CONTRACTS_H
#include "xir/xxir_view_roots_internal.h"
#include "xir/xxir_operand_roles.h"
static XrXirStatus host_file_opaque_root(void *user,const XrXirCompileContext *context,const XrXirViewRootEvent *event){
 (void)context;uint32_t *expected=user;
 CHECK(event->root_function==expected[0]&&event->root_value==expected[1]&&event->kind==XR_XIR_VIEW_ROOT_OBJECT&&event->opaque_suffix&&event->dynamic_identity&&
  !event->step_count&&!event->selection_count);++expected[2];return XR_XIR_OK;
}
static void host_file_write_rejected(XrXirCompileContext *context,const XrXirModule *module,
 const XrXirConstruction *construction,uint32_t entry,uint32_t site,const char *scenario){
 XrXirDiagnostic diagnostic={0};
 XrXirStatus ordinary=xir_compile_view_prepare_ordinary(context,(XirEffectInput){module,construction,NULL},&diagnostic);
 if(ordinary!=XR_XIR_OK){
  fprintf(stderr,"cold write ordinary scenario=%s status%u expected%u f%u b%u i%u reason%u entry%u site%u\n",
   scenario,ordinary,XR_XIR_OK,diagnostic.function,diagnostic.block,diagnostic.instruction,diagnostic.reason,entry,site);
  if(diagnostic.function<module->function_count){const XrXirFunction *body=module->functions+diagnostic.function;
   uint32_t parameters=body->parameter_count>8u?8u:body->parameter_count;
   fprintf(stderr,"cold write ordinary function=%.*s parameters%u parametersTruncated%u result%u error%u instructions%u\n",
    (int)(body->name_length>200u?200u:body->name_length),body->name?body->name:"",body->parameter_count,
    (unsigned)(parameters<body->parameter_count),body->result,body->error_type,body->instruction_count);
   for(uint32_t p=0;p<parameters;++p)fprintf(stderr,"cold write ordinary parameter%u type%u\n",p,body->parameters[p]);
   if(diagnostic.instruction<body->instruction_count){const XrXirInstruction *op=body->instructions+diagnostic.instruction;
    fprintf(stderr,"cold write ordinary op=%s type%u args=%u,%u targets=%u,%u immediate=%lld typeargs=%u,%u\n",
     xr_xir_op_name(op->op),op->type,op->args[0],op->args[1],op->targets[0],op->targets[1],(long long)op->immediate,
     op->type_arguments[0],op->type_arguments[1]);}
  }
 }
 CHECK(ordinary==XR_XIR_OK);
 XrXirStatus status=xr_xir_compile_structure_verify_v2(context,module,construction,&diagnostic);
 if(status!=XR_XIR_UNRESOLVED||diagnostic.function!=entry||diagnostic.instruction!=site)
  fprintf(stderr,"cold write footprint status%u f%u b%u i%u expected f%u i%u\n",status,
   diagnostic.function,diagnostic.block,diagnostic.instruction,entry,site);
 CHECK(status==XR_XIR_UNRESOLVED&&diagnostic.function==entry&&diagnostic.instruction==site);
}
static void host_file_write_callee_controls(XrXirCompileContext *context,const XrXirModule *module,
 const XrXirConstruction *construction,uint32_t entry,uint32_t site){
 const XrXirFunction *caller=module->functions+entry;uint32_t target=(uint32_t)caller->instructions[site].immediate;
 const XrXirFunction *body=module->functions+target;uint32_t push=UINT32_MAX,reset=UINT32_MAX;
 for(uint32_t i=0;i<body->instruction_count;++i)if(body->instructions[i].op==XR_XIR_ARRAY_PUSH){CHECK(push==UINT32_MAX);push=i;}
 for(uint32_t f=0;f<module->function_count;++f){const XrXirFunctionIdentity *id=module->declarations->functions+f;
  const XrXirFunction *candidate=module->functions+f;
  if(id->nominal_owner==module->declarations->functions[target].nominal_owner&&id->method_kind==XR_XIR_READ_METHOD&&
   candidate->name_length==5&&!memcmp(candidate->name,"reset",5)&&candidate->parameter_count==1&&
   candidate->parameters[0]==body->parameters[0]&&candidate->result==XR_XIR_UNIT&&candidate->error_type==XR_XIR_NEVER)reset=f;
 }
 CHECK(push!=UINT32_MAX&&reset!=UINT32_MAX&&body->instructions[body->instruction_count-1].op==XR_XIR_RETURN);
 XrXirFunction *functions=xr_malloc(module->function_count*sizeof(*functions));
 XrXirInstruction *ops=xr_malloc((body->instruction_count+1u)*sizeof(*ops));
 XrXirBlock *blocks=xr_malloc(body->block_count*sizeof(*blocks));
 uint32_t *operands=xr_malloc((body->operand_count+3u)*sizeof(*operands));CHECK(functions&&ops&&blocks&&operands);
 memcpy(functions,module->functions,module->function_count*sizeof(*functions));
 memcpy(ops,body->instructions,body->instruction_count*sizeof(*ops));memcpy(blocks,body->blocks,body->block_count*sizeof(*blocks));
 memcpy(operands,body->operands,body->operand_count*sizeof(*operands));
 XrXirModule changed=*module;changed.functions=functions;functions[target].instructions=ops;
 functions[target].blocks=blocks;functions[target].operands=operands;
 /* Keep the real parameters, receiver and all existing instructions. A legal
  * same-Class call after the formal view ends invalidates the complete scan. */
 uint32_t last=body->instruction_count-1u;ops[last+1u]=ops[last];operands[body->operand_count]=0;
 ops[last]=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_UNIT,.args={body->operand_count,1},.immediate=reset};
 ++functions[target].instruction_count;++functions[target].operand_count;
 CHECK(blocks[body->block_count-1u].first+blocks[body->block_count-1u].count==body->instruction_count);
 ++blocks[body->block_count-1u].count;host_file_write_rejected(context,&changed,construction,entry,site,"same-class-reset-after-cleanup");
 memcpy(ops,body->instructions,body->instruction_count*sizeof(*ops));memcpy(blocks,body->blocks,body->block_count*sizeof(*blocks));
 functions[target].instruction_count=body->instruction_count;functions[target].operand_count=body->operand_count+3u;
 /* ARRAY_SET retains legal place/index/byte geometry but is a different write. */
 CHECK(ops[push].args[1]>=body->parameter_count&&ops[push].args[1]-body->parameter_count<push);
 const XrXirInstruction *get=body->instructions+ops[push].args[1]-body->parameter_count;
 CHECK(get->op==XR_XIR_VIEW_GET&&get->args[0]==1u);
 /* Operand-table ranges follow instruction order, including the later cleanup. */
 uint32_t prefix=0;
 for(uint32_t i=0;i<push;++i)if(xr_xir_op_uses_operand_table(ops[i].op)&&ops[i].args[1]){
  CHECK(ops[i].args[0]==prefix);prefix+=ops[i].args[1];}
 CHECK(prefix<=body->operand_count);
 memcpy(operands,body->operands,prefix*sizeof(*operands));
 memcpy(operands+prefix+3u,body->operands+prefix,(body->operand_count-prefix)*sizeof(*operands));
 operands[prefix]=ops[push].args[0];operands[prefix+1u]=get->args[1];operands[prefix+2u]=ops[push].args[1];
 for(uint32_t i=push+1u;i<body->instruction_count;++i)
  if(xr_xir_op_uses_operand_table(ops[i].op)&&ops[i].args[1])ops[i].args[0]+=3u;
 ops[push]=(XrXirInstruction){.op=XR_XIR_ARRAY_SET,.type=XR_XIR_UNIT,.args={prefix,3}};
 host_file_write_rejected(context,&changed,construction,entry,site,"ARRAY_SET");
 xr_free(operands);xr_free(blocks);xr_free(ops);xr_free(functions);
}
static void host_file_write_other_owner(XrXirCompileContext *context,const XrXirModule *module,
 const XrXirConstruction *construction,uint32_t entry,uint32_t site){
 const XrXirFunction *body=module->functions+entry;const XrXirInstruction *call=body->instructions+site;
 uint32_t loan=body->operands[call->args[0]+1u],inserted=body->parameter_count+site;
 CHECK(loan>=body->parameter_count&&loan<inserted&&body->instructions[loan-body->parameter_count].op==XR_XIR_CALL);
 CHECK(site+1u<body->instruction_count&&body->instructions[site+1u].op==XR_XIR_VIEW_CLEANUP&&
  body->instructions[site+1u].args[1]==1&&body->operands[body->instructions[site+1u].args[0]]==loan);
 static const uint8_t counts[XR_XIR_OP_COUNT]={0,
#define XR_XIR_OP(name,stages,result,operands,edges,terminal) operands,
#include "xir/xxir_ops.def"
#undef XR_XIR_OP
 };
 XrXirFunction *functions=xr_malloc(module->function_count*sizeof(*functions));
 XrXirInstruction *ops=xr_malloc((body->instruction_count+1u)*sizeof(*ops));
 XrXirBlock *blocks=xr_malloc(body->block_count*sizeof(*blocks));
 uint32_t *operands=xr_malloc((body->operand_count+2u)*sizeof(*operands));CHECK(functions&&ops&&blocks&&operands);
 memcpy(functions,module->functions,module->function_count*sizeof(*functions));memcpy(blocks,body->blocks,body->block_count*sizeof(*blocks));
 for(uint32_t p=0;p<body->operand_count;++p)operands[p]=body->operands[p]+(body->operands[p]>=inserted);
 for(uint32_t i=0;i<body->instruction_count;++i){XrXirInstruction op=body->instructions[i];
  CHECK(op.op!=XR_XIR_PHI&&op.op!=XR_XIR_INVOKE_RESULT&&!xr_xir_op_uses_value_path(body,&op));
  if(!xr_xir_op_uses_operand_table(op.op)){uint32_t count=op.op==XR_XIR_RETURN?1u:counts[op.op];CHECK(count<=2);
   for(uint32_t p=0;p<count;++p)if(op.args[p]>=inserted)++op.args[p];}
  ops[i+(i>=site)]=op;
 }
 ops[site]=body->instructions[loan-body->parameter_count];
 for(uint32_t b=0;b<body->block_count;++b){CHECK(!blocks[b].frontier);
  if(blocks[b].first>site)++blocks[b].first;
  else if(site>=blocks[b].first&&site-blocks[b].first<blocks[b].count)++blocks[b].count;}
 /* Both genuine readonly Buffer results stay alive until this cleanup. Only
  * the original one is the write argument; the other owner must still reject. */
 operands[body->operand_count]=loan;operands[body->operand_count+1u]=inserted;
 ops[site+2u].args[0]=body->operand_count;ops[site+2u].args[1]=2;
 XrXirModule changed=*module;changed.functions=functions;functions[entry].instructions=ops;functions[entry].blocks=blocks;
 functions[entry].operands=operands;functions[entry].operand_count=body->operand_count+2u;++functions[entry].instruction_count;
 host_file_write_rejected(context,&changed,construction,entry,site+1u,"other-live-owner");
 xr_free(operands);xr_free(blocks);xr_free(ops);xr_free(functions);
}
static void host_file_write_boundary_controls(XrXirCompileContext *context,const XrXirModule *module,
 const XrXirConstruction *construction,uint32_t entry,uint32_t borrow_site){
 const XrXirFunction *body=module->functions+entry;uint32_t loan=body->parameter_count+borrow_site,site=UINT32_MAX;
 for(uint32_t i=borrow_site+1u;i<body->instruction_count;++i){const XrXirInstruction *op=body->instructions+i;
  if(op->op==XR_XIR_CALL&&op->args[1]==2&&body->operands[op->args[0]+1u]==loan){CHECK(site==UINT32_MAX);site=i;}}
 CHECK(site!=UINT32_MAX);XirViewRootResolver *resolver=NULL;
 CHECK(xir_compile_view_root_resolver_new(context,(XirEffectInput){module,construction,NULL},&resolver,NULL)==XR_XIR_OK);
 uint32_t receiver=body->operands[body->instructions[borrow_site].args[0]];
 uint32_t expected[3]={entry,receiver,0};XirViewRootRequest request={entry,receiver,site,NULL,0};
 CHECK(xir_compile_view_root_resolver_query(resolver,&request,host_file_opaque_root,expected,NULL)==XR_XIR_OK&&expected[2]==1);
 xir_compile_view_root_resolver_free(resolver);
 host_file_write_callee_controls(context,module,construction,entry,site);
 host_file_write_other_owner(context,module,construction,entry,site);
}
#endif // HOST_OS_IO_FILE_WRITE_CONTRACTS_H
