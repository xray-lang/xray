/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_inline_source_runtime.c - Counted direct Source Checked lifetime
 *
 * KEY CONCEPT:
 *   The first owned Checked result survives the source snapshot/session and executes directly.
 */
#include "xir/xxir_vm.h"
#include "xir/xxir_class.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CHECK(x) C(x)
#include "xir_runtime_allocations.h"
#include "xir_class_inline_retained.h"
XR_FUNC size_t xr_test_class_inline_runtime_live(void){return runtime_live;}
XR_FUNC size_t xr_test_class_inline_runtime_bytes(void){return runtime_bytes;}
static void class_runtime_faults(XrXirProgram *program) {
 size_t base=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirValue value={0};
 XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,3,NULL,0);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;
 if(status==XR_XIR_CALL_RETURNED){C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);C(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);
 status=xr_xir_instance_start(instance,4,NULL,0);if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;
 if(status==XR_XIR_CALL_RETURNED)C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);}
 if(!pass){C(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;C(sites>0);}else C(status==XR_XIR_CALL_OOM);
 if(instance)C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
 C(runtime_live==base && runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("class runtime OOM sites %zu physical baseline restored\n",sites);
}
XR_FUNC void xr_test_class_inline_source_run(XrXirArtifact **checked){
 C(checked&&*checked);C(xr_xir_artifact_verify(*checked,NULL,NULL)==XR_XIR_OK);
 XrXirArtifact *special=NULL,*lowered=NULL;
 C(xr_xir_specialize(*checked,NULL,&special,NULL)==XR_XIR_OK);
 xr_xir_artifact_free(*checked);*checked=NULL;
 C(xr_xir_artifact_verify(special,NULL,NULL)==XR_XIR_OK);
 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 C(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
 const XrXirModule *module=xr_xir_artifact_module(lowered);uint32_t answer=UINT32_MAX,retained=UINT32_MAX;
 C(module->function_count==25);
 for(uint32_t f=0;f<module->function_count;++f){
  if(module->functions[f].name_length==6&&!memcmp(module->functions[f].name,"answer",6)){C(answer==UINT32_MAX);answer=f;}
  if(module->functions[f].name_length==8&&!memcmp(module->functions[f].name,"retained",8)){C(retained==UINT32_MAX);retained=f;}
 }
 C(answer==3&&retained==4);XrXirProgram *program=NULL;
 C(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
 class_runtime_faults(program);
 XrXirInstance *instances[2]={NULL,NULL};XrXirValue held[2]={{0},{0}};
 XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
 for(unsigned i=0;i<2;++i)C(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);
 for(unsigned i=0;i<2;++i){
  C(xr_xir_instance_start(instances[i],answer,NULL,0)==XR_XIR_CALL_READY);
  C(xr_xir_instance_poll(instances[i]).outcome.status==XR_XIR_CALL_RETURNED);
  XrXirValue value={0};C(xr_xir_instance_take_result(instances[i],&value)==XR_XIR_CALL_RETURNED);
  C(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);
  C(xr_xir_instance_start(instances[i],retained,NULL,0)==XR_XIR_CALL_READY);
  C(xr_xir_instance_poll(instances[i]).outcome.status==XR_XIR_CALL_RETURNED);
  C(xr_xir_instance_take_result(instances[i],&held[i])==XR_XIR_CALL_RETURNED);
 }
 for(unsigned i=0;i<2;++i)C(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
 xr_xir_program_drop(program);
 for(unsigned i=0;i<2;++i)class_inline_retained(&held[i]);
 C(!runtime_live&&!runtime_bytes);
 puts("direct Source Checked: inline class VM41, retained GET4 new domain, physical zero PASS");
}
