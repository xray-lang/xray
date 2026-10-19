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
#define CONSUMER_KIND 3
#include "xir_library_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_specialize.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_library_catalog.c"
XR_FUNC size_t *xr_test_library_runtime_counter(unsigned index) {
 CHECK(index<4);size_t *values[]={&runtime_attempts,&runtime_fail_at,&runtime_live,&runtime_bytes};return values[index];
}
static bool pair(XrXirProgram *program,uint32_t entry){
 for(unsigned i=0;i<2;++i){XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirInstance *instance=NULL;
  XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
  if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,entry,NULL,0);
  if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
  if(status==XR_XIR_CALL_RETURNED){XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
   CHECK(result.type==XR_XIR_I64&&result.payload==41);xr_xir_value_drop(&result);
  }else CHECK(status==XR_XIR_CALL_OOM);
  if(instance)CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
  if(status!=XR_XIR_CALL_RETURNED)return false;
 }return true;
}
XR_FUNC void xr_test_library_source_run(XrXirArtifact *owned) {
 CHECK(xr_xir_compile_artifact_verify(owned,NULL)==XR_XIR_OK);
 const XrXirModule *original=xr_xir_compile_artifact_module(owned);
 uint32_t defaults=original->defaults?original->defaults->count:0;
 XrXirArtifact *specialized=NULL,*lowered=NULL;
 CHECK(xr_xir_compile_specialize(owned,&specialized,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(owned);
 CHECK(xr_xir_compile_artifact_verify(specialized,NULL)==XR_XIR_OK);
 const XrXirModule *projected=xr_xir_compile_artifact_module(specialized);CHECK(!projected->defaults);
 if(defaults){CHECK(projected->provenance&&projected->provenance->source->module.defaults);
  CHECK(projected->provenance->source->module.defaults->count==defaults);}

 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
 CHECK(xr_xir_compile_lower(specialized,&target,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(specialized);
 const XrXirModule *executable=xr_xir_compile_artifact_module(lowered);uint32_t entry=UINT32_MAX;
 for(uint32_t f=0;f<executable->function_count;++f)if(executable->declarations->functions[f].module==executable->declarations->root_module &&
     executable->functions[f].name_length==6&&!memcmp(executable->functions[f].name,"result",6)) {CHECK(entry==UINT32_MAX);entry=f;}
 CHECK(entry!=UINT32_MAX);
 XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
 runtime_attempts=0;CHECK(pair(program,entry));size_t sites=runtime_attempts,live=runtime_live,bytes=runtime_bytes;
 for(size_t f=0;f<sites;++f){runtime_attempts=0;runtime_fail_at=f;CHECK(!pair(program,entry));CHECK(runtime_live==live&&runtime_bytes==bytes);}
 runtime_fail_at=SIZE_MAX;xr_xir_compile_program_drop(program);CHECK(!runtime_live&&!runtime_bytes);
 printf("direct Source Checked VM41 two instances runtime OOM %zu physical zero\n",sites);
}
