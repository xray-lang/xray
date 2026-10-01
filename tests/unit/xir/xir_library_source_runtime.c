/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_source_runtime.c - Counted direct Source runtime
 *
 * KEY CONCEPT:
 *   The original owned Checked survives every source producer before execution.
 */
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#include "xir_runtime_allocations.h"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_library_catalog.c"
XR_FUNC size_t *xr_test_library_runtime_counter(unsigned index) {
 CHECK(index<4);size_t *values[]={&runtime_attempts,&runtime_fail_at,&runtime_live,&runtime_bytes};return values[index];
}
static bool pair(XrXirProgram *program){
 for(unsigned i=0;i<2;++i){XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
  XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
  if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,2,NULL,0);
  if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll(instance).outcome.status;
  if(status==XR_XIR_CALL_RETURNED){XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
   CHECK(result.type==XR_XIR_I64&&result.payload==41);xr_xir_value_drop(&result);
  }else CHECK(status==XR_XIR_CALL_OOM);
  if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
  if(status!=XR_XIR_CALL_RETURNED)return false;
 }return true;
}
XR_FUNC void xr_test_library_source_run(XrXirArtifact *owned) {
 CHECK(xr_xir_artifact_verify(owned,NULL,NULL)==XR_XIR_OK);
 const XrXirModule *original=xr_xir_artifact_module(owned);
 uint32_t defaults=original->defaults?original->defaults->count:0;
 XrXirArtifact *specialized=NULL,*lowered=NULL;
 CHECK(xr_xir_specialize(owned,NULL,&specialized,NULL)==XR_XIR_OK);xr_xir_artifact_free(owned);
 CHECK(xr_xir_artifact_verify(specialized,NULL,NULL)==XR_XIR_OK);
 const XrXirModule *projected=xr_xir_artifact_module(specialized);CHECK(!projected->defaults);
 if(defaults){CHECK(projected->provenance&&projected->provenance->source->module.defaults);
  CHECK(projected->provenance->source->module.defaults->count==defaults);}

 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 CHECK(xr_xir_lower(specialized,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(specialized);
 XrXirProgram *program=NULL;CHECK(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){1048576,1048576},&program)==XR_XIR_OK);
 runtime_attempts=0;CHECK(pair(program));size_t sites=runtime_attempts,live=runtime_live,bytes=runtime_bytes;
 for(size_t f=0;f<sites;++f){runtime_attempts=0;runtime_fail_at=f;CHECK(!pair(program));CHECK(runtime_live==live&&runtime_bytes==bytes);}
 runtime_fail_at=SIZE_MAX;xr_xir_program_drop(program);CHECK(!runtime_live&&!runtime_bytes);
 printf("direct Source Checked VM41 two instances runtime OOM %zu physical zero\n",sites);
}
