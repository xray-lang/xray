/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_packet.c - Real class identity execution and lifetime gates
 *
 * KEY CONCEPT:
 *   One Checked source graph feeds independent VM and native expected results.
 */
#include "xir/xxir_class.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CHECK(x) C(x)
#include "xir_runtime_allocations.h"
#include "xir_class_deep_cases.h"
#include "xir_class_enum_deep_cases.h"
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
int main(int argc,char **argv){
 class_deep_cases();class_cross_arena_cases();class_exit_cases();class_program_retained_case();class_enum_deep_cases();
 C(argc==1 || argc==2);FILE *file=fopen(XR_CHECKED_FIXTURE,"rb");C(file);
 C(!fseek(file,0,SEEK_END));long size=ftell(file);C(size>=64 && size<=262144);C(!fseek(file,0,SEEK_SET));
 uint8_t *bytes=malloc((size_t)size);C(bytes);C(fread(bytes,1,(size_t)size,file)==(size_t)size);C(!fclose(file));
 XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;
 C(xr_xir_checked_read(bytes,(size_t)size,NULL,&checked,NULL)==XR_XIR_OK);free(bytes);
 C(xr_xir_specialize(checked,NULL,&special,NULL)==XR_XIR_OK);xr_xir_artifact_free(checked);
 C(xr_xir_artifact_verify(special,NULL,NULL)==XR_XIR_OK);
 XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};C(xr_xir_lower(special,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_artifact_free(special);
 const XrXirModule *module=xr_xir_artifact_module(lowered);C(module->function_count==11);
 C(module->functions[3].name_length==6 && !memcmp(module->functions[3].name,"answer",6));
 C(module->functions[4].name_length==8 && !memcmp(module->functions[4].name,"retained",8));
 XrXirCSource output={0};C(xr_xir_emit_c(lowered,"source_class",1048576,&output)==XR_XIR_OK);
 if(argc==2){file=fopen(argv[1],"wb");C(file);C(fwrite(output.text,1,output.length,file)==output.length);C(!fclose(file));}xr_xir_c_source_free(&output);
 XrXirProgram *program=NULL;C(xr_xir_vm_program_take(&lowered,(XrXirProgramBudget){33554432,64000000},&program)==XR_XIR_OK);
 class_runtime_faults(program);
 XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
 C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 C(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64 && number.payload==41);xr_xir_value_drop(&number);
 C(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 xr_xir_program_drop(program);xr_xir_artifact_free(lowered);
 for(unsigned i=0;i<2;++i){XrXirValue value={0};C(xr_xir_class_get(&saved[i],0,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&saved[i]),.work=10000},&value)==XR_XIR_VALUE_OK && value.type==XR_XIR_I64 && value.payload==40);xr_xir_value_drop(&value);
 C(xr_xir_class_get(&saved[i],1,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&saved[i]),.work=10000},&value)==XR_XIR_VALUE_OK);const char *text=NULL;size_t n=0;C(xr_xir_string_view(&value,&text,&n)&&n==7&&!memcmp(text,"counter",7));xr_xir_value_drop(&saved[i]);C(xr_xir_string_view(&value,&text,&n)&&n==7);xr_xir_value_drop(&value);}
 C(!runtime_live && !runtime_bytes);
 puts("source-free packet VM class41 retained40/string physical release PASS");return 0;
}
