/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_iteration_execution.c - Real Array iteration execution and lifetime gates
 *
 * KEY CONCEPT:
 *   One Checked source graph feeds independent VM and native expected results.
 */
#include "xir/xxir_class.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CHECK(x) C(x)
#include "xir_runtime_allocations.h"
#include "xir_iteration_runtime_cases.h"
int main(int argc,char **argv){
 C(argc==1 || argc==2);FILE *file=fopen(XR_CHECKED_FIXTURE,"rb");C(file);
 C(!fseek(file,0,SEEK_END));long size=ftell(file);C(size>=64 && size<=262144);C(!fseek(file,0,SEEK_SET));
 uint8_t *bytes=malloc((size_t)size);C(bytes);C(fread(bytes,1,(size_t)size,file)==(size_t)size);C(!fclose(file));
 XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;
 C(xr_xir_checked_read(bytes,(size_t)size,NULL,&checked,NULL)==XR_XIR_OK);free(bytes);
 C(xr_xir_specialize(checked,NULL,&special,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
 C(xr_xir_artifact_verify(special,NULL,NULL)==XR_XIR_OK);
 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};C(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
 const XrXirModule *module=xr_xir_artifact_module(lowered);C(module->function_count==26);
 C(module->functions[3].name_length==6 && !memcmp(module->functions[3].name,"answer",6));
 C(module->functions[4].name_length==8 && !memcmp(module->functions[4].name,"retained",8));
 XrXirCSource output={0};C(xr_xir_emit_c(lowered,"source_iteration",1048576,&output)==XR_XIR_OK);
 if(argc==2){file=fopen(argv[1],"wb");C(file);C(fwrite(output.text,1,output.length,file)==output.length);C(!fclose(file));}xr_xir_c_source_free(&output);
 XrXirProgram *program=NULL;C(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
 iteration_runtime_faults(program);
 iteration_exit_allocations(program);
 XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
 C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 C(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64 && number.payload==41);xr_xir_value_drop(&number);
 C(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 for(unsigned cancel=0;cancel<2;++cancel){
 XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);IterationTrace trace={0};config.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, iteration_trace, &trace};
 C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 C(xr_xir_instance_start(instance,5,NULL,0)==XR_XIR_CALL_READY);
 XrXirInstanceResult resumed=xr_xir_instance_poll_bounded(instance, UINT64_MAX);C(resumed.outcome.status==XR_XIR_CALL_SUSPENDED);
 if(!cancel){unsigned pauses=1;while(resumed.outcome.status==XR_XIR_CALL_SUSPENDED){C(pauses<=3);C(xr_xir_instance_resume(instance,resumed.epoch,resumed.outcome.wake)==XR_XIR_CALL_READY);resumed=xr_xir_instance_poll_bounded(instance, UINT64_MAX);++pauses;}C(pauses==4 && resumed.outcome.status==XR_XIR_CALL_RETURNED);XrXirValue n={0};C(xr_xir_instance_take_result(instance,&n)==XR_XIR_CALL_RETURNED && n.type==XR_XIR_I64 && n.payload==41);xr_xir_value_drop(&n);}
 C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);C(trace.count==(cancel?1u:3u));
 }
 xr_xir_program_drop(program);xr_xir_artifact_free(lowered);
 for(unsigned i=0;i<2;++i){const char *text=NULL;size_t n=0;C(xr_xir_string_view(&saved[i],&text,&n)&&n==6&&!memcmp(text,"mapped",6));xr_xir_value_drop(&saved[i]);}
 C(!runtime_live && !runtime_bytes);
 puts("source-free packet Array iteration VM41 retained string PASS");return 0;
}
