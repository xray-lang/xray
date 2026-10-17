/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_inline_execution.c - Real class identity execution and lifetime gates
 *
 * KEY CONCEPT:
 *   One Checked source graph feeds independent VM and native expected results.
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_class.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CHECK(x) C(x)
#include "xir_class_pipeline_owner.h"
#include "xir_runtime_allocations.h"
extern const XrXirProgramSpec source_class_inline_program;
#include "xir_class_owned_receive.h"
static void class_runtime_faults(XrXirProgram *program) {
 size_t base=runtime_live,bytes=runtime_bytes,sites=0;
 for(size_t pass=0;pass<=sites;++pass){runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
 XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);XrXirValue value={0};
 XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_start(instance,3,NULL,0);
 if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
 if(status==XR_XIR_CALL_RETURNED){C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);C(value.type==XR_XIR_I64&&value.payload==41);xr_xir_value_drop(&value);
 status=xr_xir_instance_start(instance,4,NULL,0);if(status==XR_XIR_CALL_READY)status=xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
 if(status==XR_XIR_CALL_RETURNED)C(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);}
 if(!pass){C(status==XR_XIR_CALL_RETURNED);sites=runtime_attempts;C(sites>0);}else C(status==XR_XIR_CALL_OOM);
 if(instance)C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_value_drop(&value);
 C(runtime_live==base && runtime_bytes==bytes);
 }
 runtime_fail_at=SIZE_MAX;printf("class runtime OOM sites %zu physical baseline restored\n",sites);
}
#include "xir_class_inline_retained.h"
int main(int argument_count,char **arguments){
 bool owned_recovery=argument_count==2 && !strcmp(arguments[1],"--owned-recovery");
 bool compiler_faults=argument_count==2 && !strcmp(arguments[1],"--owned-compiler-faults");
 C(argument_count==1 || owned_recovery || compiler_faults);
 C(argument_count==1 || XR_CLASS_INLINE_MIXED);
 if(compiler_faults){
  ClassOwnedFixture fixture={&source_class_inline_program};
  class_pipeline_faults("Owned class mixed reception",class_owned_operation,&fixture);
  class_pipeline_final_zero();return 0;
 }
 ClassPipelineOwner owner={0};C(class_pipeline_new(&owner,&class_pipeline_limits)==XR_XIR_OK);
 const int argc=XR_CLASS_INLINE_MIXED?2:1;XrXirProgramSpec spec=source_class_inline_program;XrXirProgram *program=NULL;
 XrXirProgramSpec old=spec;old.abi_version=23;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
 C(xr_xir_compile_program_seal(&owner.context,&old,&program)==XR_XIR_BAD_LAYOUT && !program);
 old=spec;old.target.abi_version=13;old.declarations=(const XrXirDeclarations *)(uintptr_t)1;
 C(xr_xir_compile_program_seal(&owner.context,&old,&program)==XR_XIR_BAD_LAYOUT && !program);
 uint8_t *packet=malloc(spec.packet.length);C(packet);memcpy(packet,spec.packet.bytes,spec.packet.length);
 for(unsigned mode=0;mode<2;++mode){memcpy(packet,spec.packet.bytes,spec.packet.length);packet[mode?12:8]=mode?46:18;
 XrSHA256Context sha;xr_sha256_init(&sha);xr_sha256_update(&sha,packet,32);xr_sha256_update(&sha,packet+64,spec.packet.length-64);xr_sha256_final(&sha,packet+32);
 XrXirArtifact *rejected=NULL;C(xr_xir_compile_checked_read(&owner.context,packet,spec.packet.length,&rejected,NULL)==XR_XIR_BAD_STRUCTURE && !rejected);}free(packet);
 if(argc>1){C(spec.entry_count<=32);
  C(class_owned_program(&owner.context,&spec,owned_recovery,&program)==XR_XIR_OK);
 }else C(xr_xir_compile_program_seal(&owner.context,&spec,&program)==XR_XIR_OK);
 if(!owned_recovery)class_runtime_faults(program);
 XrXirValue saved[2]={{0},{0}};
 XrXirInstance *retained_instances[2]={NULL,NULL};
 if(owned_recovery){
  for(unsigned i=0;i<2;++i){XrXirInstanceConfig config;
   C(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
   C(xr_xir_instance_new(program,&config,&retained_instances[i])==XR_XIR_CALL_READY);
  }
  class_pipeline_release_producer(&owner);
  xr_xir_compile_program_drop(program);program=NULL;C(!class_code_releases);
 }
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=retained_instances[i];XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
 C(owned_recovery || xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 C(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64 && number.payload==41);xr_xir_value_drop(&number);
 C(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
 if(owned_recovery)C(class_code_releases==(i==1?1u:0u));}
 if(!owned_recovery){C(!class_code_releases);xr_xir_compile_program_drop(program);
  C(class_code_releases==(argc>1?1u:0u));
 }else C(!program && class_code_releases==1);
 for(unsigned i=0;i<2;++i)class_inline_retained(&saved[i]);
 C(!runtime_live && !runtime_bytes);
 puts(argc>1?"class source mixed41 retained40/string oldABI PASS":"class source native41 retained40/string oldABI PASS");class_pipeline_drop(&owner);class_pipeline_final_zero();return 0;
}
