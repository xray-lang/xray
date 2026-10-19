/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_physical_replay_cases.h - Fresh physical Class input and complete proof failures
 */
#ifndef XIR_CLASS_PHYSICAL_REPLAY_CASES_H
#define XIR_CLASS_PHYSICAL_REPLAY_CASES_H
#include "xir_class_physical_scalar_cases.h"
static XrXirStatus conditional_class_physical_trial(const XrXirCompileContext *context,uint32_t mode,size_t fail,bool compare,size_t *sites){
 InvocationClassCellFixture fixture;invocation_class_cell_fixture(&fixture,mode);
 XrXirArtifact *checked=NULL;XrXirEffects *effects=NULL;EffectInvocationRead read={0};
 XrXirStatus status=xir_fixture_check(context,&fixture.source.module,&checked,NULL);
 if(status==XR_XIR_OK)status=xr_xir_compile_effects_analyze(checked,&effects);
 xr_xir_compile_artifact_free(checked);memset(&fixture,0xCE,sizeof(fixture));
 if(status==XR_XIR_OK){
  EffectInvocationCertificate *certificate=effects->invocations;
  status=effect_invocation_read_open(context,effects,certificate->declared,certificate->final,&read);
  if(status==XR_XIR_OK){
   read.work=context;read.effects=effects;read.declared=certificate->declared;
   EffectBodyRequest body=effect_body_owned_request(context,certificate->body);
   if(compare)CHECK(oracle_effect_invocation_class_verify_read(&read,&body)==XR_XIR_OK);
   RootParameterMark held=rp_mark();uint64_t live=rp_stats(context).live_bytes;size_t start=rp_attempts;rp_injected=false;
   rp_fail_at=fail==SIZE_MAX?SIZE_MAX:start+fail;status=effect_invocation_class_verify_read(&read,&body);
   rp_fail_at=SIZE_MAX;if(sites)*sites=rp_attempts-start;
   CHECK(rp_stats(context).live_bytes==live);rp_balanced(held);
   if(fail!=SIZE_MAX)CHECK(status==XR_XIR_OUT_OF_MEMORY && rp_injected && rp_attempts-start==fail+1);
   if(compare && status==XR_XIR_OK){
    uint32_t selected=read.final->roots[3];EffectInvocationNode *node=&read.final->nodes[selected];
    CHECK(node->class_count && node->classes);EffectInvocationClassFact saved=node->classes[0];node->classes[0].mask=0;
    CHECK(oracle_effect_invocation_class_verify_read(&read,&body)==XR_XIR_BAD_STRUCTURE && effect_invocation_class_verify_read(&read,&body)==XR_XIR_BAD_STRUCTURE);node->classes[0]=saved;
    EffectCellReplayRows rows={0};CHECK(effect_cell_replay_rows_create(context,&body,&rows)==XR_XIR_OK);
    EffectInvocationFlow flow={.read=&read,.node=selected};CHECK(effect_invocation_origins_read(&flow,&body)==XR_XIR_OK);uint64_t limit=0;
    CHECK(effect_invocation_class_rank_limit_count(&read,body.domain.function_count,&limit)==XR_XIR_OK);
    XrXirFunction wrong=rows.functions[node->body];++wrong.parameter_count;
    CHECK(effect_invocation_class_body_match(&flow,limit,&wrong)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_invocation_class_body_match(&flow,limit,&rows.functions[node->body])==XR_XIR_OK);
    xr_compile_resources_free(flow.memory);effect_cell_replay_rows_free(&rows);
    CHECK(effect_invocation_class_verify_read(&read,&body)==XR_XIR_OK);
   }
  }
 }
 effect_invocation_read_close(&read);xr_xir_compile_effects_free(effects);return status;
}
static void conditional_class_physical_tests(void){
 conditional_class_rows_literals();conditional_class_cell_literals();
 for(uint32_t mode=0;mode<7;++mode){
  RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;size_t sites=0;
  CHECK(conditional_class_physical_trial(&context,mode,SIZE_MAX,true,&sites)==XR_XIR_OK);rp_owner_free(&context,baseline);rp_balanced(physical);CHECK(sites);
  printf("CLASS_ROWS mode%u scoped_sites%zu old_scalar/poisonedproducer/mask/dimension PASS\n",mode,sites);
  for(size_t fail=0;fail<sites;++fail){context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;CHECK(conditional_class_physical_trial(&context,mode,fail,false,NULL)==XR_XIR_OUT_OF_MEMORY);rp_owner_free(&context,baseline);rp_balanced(physical);}
  context=rp_owner(rp_caps());baseline=rp_stats(&context).live_bytes;CHECK(conditional_class_physical_trial(&context,mode,SIZE_MAX,false,NULL)==XR_XIR_OK);XrCompileResourceStats stats=rp_stats(&context);rp_owner_free(&context,baseline);rp_balanced(physical);
  uint64_t values[3]={stats.allocated_bytes,stats.peak_bytes,stats.work};
  for(uint32_t axis=0;axis<3;++axis)for(uint32_t minus=0;minus<2;++minus){XrCompileResourceLimits caps=rp_caps();uint64_t v=values[axis]-minus;if(!axis)caps.allocated_bytes=v;else if(axis==1)caps.live_bytes=v;else caps.work=v;context=rp_owner(caps);baseline=rp_stats(&context).live_bytes;CHECK(conditional_class_physical_trial(&context,mode,SIZE_MAX,false,NULL)==(minus?XR_XIR_BUDGET:XR_XIR_OK));rp_owner_free(&context,baseline);rp_balanced(physical);}
 }
 CHECK(!rp_live&&!rp_live_bytes);puts("Fresh complete physical Class input oldscalar/negatives/allscopedOOM/exactaxes/physical0 PASS");
}
#endif // XIR_CLASS_PHYSICAL_REPLAY_CASES_H
