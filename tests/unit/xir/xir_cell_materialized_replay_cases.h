/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_materialized_replay_cases.h - Complete physical Cell replay and failure ownership
 */
#ifndef XIR_CELL_MATERIALIZED_REPLAY_CASES_H
#define XIR_CELL_MATERIALIZED_REPLAY_CASES_H
static XrXirStatus conditional_materialized_original(const EffectInvocationRead *owner,const EffectBodyRequest *body) {
 EffectCellBodyAdapter adapter={0};XrXirCellProofBodyRead read={0};
 XrXirStatus status=effect_cell_body_request(owner->work,body,&adapter,&read);
 return status==XR_XIR_OK?xr_xir_compile_cell_provenance_body_matches(owner->work,owner->final->cells,&read,NULL):status;
}
static XrXirStatus conditional_materialized_case(const XrXirCompileContext *context,uint32_t mode,size_t fail,bool oracle,size_t *sites) {
 CellIndexFixture fixture;ci_fixture(&fixture,mode==1?CI_PHI:mode==2?CI_INVOKE_UNKNOWN:CI_CALL,false,mode==0);
 if(mode==3){fixture.nodes[1].element=XR_XIR_UNIT;fixture.subject[0]=(XrXirInstruction){.op=XR_XIR_CELL_NEW,.type=(XrXirType)CI_CELL};fixture.subject[1]=(XrXirInstruction){.op=XR_XIR_CELL_READ,.args={0}};fixture.subject[2]=fixture.init;fixture.blocks[0].count=3;fixture.functions[1].parameters=NULL;fixture.functions[1].parameter_count=0;fixture.functions[1].instruction_count=3;}
 XrXirCellProvenance *independent=NULL;EffectBodyMaterial *material=NULL;
 XrXirStatus status=xir_fixture_verify(context,&fixture.module,NULL);
 if(status==XR_XIR_OK)status=xr_xir_compile_cell_provenance_verified(context,&fixture.module,&independent,NULL);
 if(status==XR_XIR_OK)status=effect_body_material_create(context,&fixture.module,&material);
 memset(&fixture,0xCE,sizeof(fixture));
 if(status==XR_XIR_OK){
  CHECK(xir_cell_provenance_module_matches(NULL,independent,NULL,NULL)==XR_XIR_BAD_STRUCTURE);
  XrXirModule missing={.function_count=1};
  CHECK(xir_cell_provenance_module_matches(context,independent,&missing,NULL)==XR_XIR_BAD_STRUCTURE);
  XrXirEffects effects={.resources=context->resources,.count=effect_body_count(material)};
  EffectInvocationFinal final={.cells=independent};EffectInvocationRead owner={.work=context,.effects=&effects,.final=&final};
  EffectBodyRequest body=effect_body_owned_request(context,material);
  if(oracle) CHECK(conditional_materialized_original(&owner,&body)==XR_XIR_OK);
  RootParameterMark held=rp_mark();uint64_t live=rp_stats(context).live_bytes;size_t start=rp_attempts;rp_injected=false;
  rp_fail_at=fail==SIZE_MAX?SIZE_MAX:start+fail;
  status=effect_invocation_class_cells_match(&owner,&body);
  rp_fail_at=SIZE_MAX;if(sites)*sites=rp_attempts-start;
  CHECK(rp_stats(context).live_bytes==live);rp_balanced(held);
  if(fail!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-start==fail+1);
  if(oracle && status==XR_XIR_OK){
   EffectBodyRequest changed=body;changed.domain.function_count=0;CHECK(conditional_materialized_original(&owner,&changed)==XR_XIR_BAD_STRUCTURE && effect_invocation_class_cells_match(&owner,&changed)==XR_XIR_BAD_STRUCTURE);
   XrXirCompileContext other=*context;changed=body;changed.read.owner=&other;CHECK(conditional_materialized_original(&owner,&changed)==XR_XIR_BAD_STRUCTURE && effect_invocation_class_cells_match(&owner,&changed)==XR_XIR_BAD_STRUCTURE);
   EffectBodyFunction *function=&material->functions[1];uint32_t runs=function->run_count;function->run_count=runs+1;CHECK(conditional_materialized_original(&owner,&body)==XR_XIR_BAD_STRUCTURE && effect_invocation_class_cells_match(&owner,&body)==XR_XIR_BAD_STRUCTURE);function->run_count=runs;
   for(uint32_t r=0;r<runs;++r){EffectBodyRun *run=&function->runs[r];uint32_t mask=run->mask;for(uint32_t w=0;w<10;++w)if(mask&(1u<<w)){uint32_t saved=run->delta[w];run->delta[w]=0;CHECK(conditional_materialized_original(&owner,&body)==XR_XIR_BAD_STRUCTURE && effect_invocation_class_cells_match(&owner,&body)==XR_XIR_BAD_STRUCTURE);run->delta[w]=saved;}}
   CHECK(effect_invocation_class_cells_match(&owner,&body)==XR_XIR_OK);
  }
 }
 xr_xir_compile_cell_provenance_free(independent);effect_body_material_free(material);return status;
}
static void conditional_materialized_replay_tests(void){
 for(uint32_t mode=0;mode<4;++mode){
  RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());context.limits.instructions=CI_MAX_OPS;uint64_t baseline=rp_stats(&context).live_bytes;size_t sites=0;
  CHECK(conditional_materialized_case(&context,mode,SIZE_MAX,true,&sites)==XR_XIR_OK);
  rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(sites);
  printf("MATERIALIZED mode%u operation_sites%zu raw_oracle/poisonedproducer/guards/delta/EOF PASS\n",mode,sites);
  for(size_t fail=0;fail<sites;++fail){context=rp_owner(rp_caps());context.limits.instructions=CI_MAX_OPS;baseline=rp_stats(&context).live_bytes;CHECK(conditional_materialized_case(&context,mode,fail,false,NULL)==XR_XIR_OUT_OF_MEMORY);rp_owner_free(&context,baseline);rp_balanced(physical);}
  context=rp_owner(rp_caps());context.limits.instructions=CI_MAX_OPS;baseline=rp_stats(&context).live_bytes;CHECK(conditional_materialized_case(&context,mode,SIZE_MAX,false,NULL)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
  uint64_t measured[3]={stats.allocated_bytes,stats.peak_bytes,stats.work};
  for(uint32_t axis=0;axis<3;++axis)for(uint32_t minus=0;minus<2;++minus){XrCompileResourceLimits caps=rp_caps();uint64_t limit=measured[axis]-minus;if(!axis)caps.allocated_bytes=limit;else if(axis==1)caps.live_bytes=limit;else caps.work=limit;context=rp_owner(caps);context.limits.instructions=CI_MAX_OPS;baseline=rp_stats(&context).live_bytes;CHECK(conditional_materialized_case(&context,mode,SIZE_MAX,false,NULL)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);}
 }
 CHECK(!rp_live&&!rp_live_bytes);puts("Materialized complete Cell replay all operation OOM and exact/minus1 axes PASS");
}
#endif // XIR_CELL_MATERIALIZED_REPLAY_CASES_H
