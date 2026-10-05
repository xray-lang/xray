/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_array_packet.c - Real class identity execution and lifetime gates
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
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_class_array_compile_owner.h"
#include "xir_class_array_contract_cases.h"
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
#include "xir_class_array_retained.h"
static const uint8_t *class_input_bytes;static size_t class_input_length;static const char *class_c_path;
static XrXirStatus class_packet_build(const XrXirCompileContext *context,unsigned variant,XrXirProgram **output) {
    (void)variant;XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;XrXirCSource source={0};
    XrXirStatus status=xr_xir_compile_checked_read(context,class_input_bytes,class_input_length,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&special,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(special,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    if(status==XR_XIR_OK){const XrXirModule *module=xr_xir_compile_artifact_module(lowered);C(module->function_count>5 && module->function_count<=32);
        C(module->functions[3].name_length==6 && !memcmp(module->functions[3].name,"answer",6));
        C(module->functions[4].name_length==8 && !memcmp(module->functions[4].name,"retained",8));}
    if(status==XR_XIR_OK)status=xr_xir_compile_emit_c(lowered,"source_class_array",1048576,&source);
    if(status==XR_XIR_OK){C(source.text[source.length]==0 && !strstr(source.text,"({"));
        if(class_c_path){FILE *file=fopen(class_c_path,"wb");C(file);C(fwrite(source.text,1,source.length,file)==source.length);C(!fclose(file));}}
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,output);
    if(status!=XR_XIR_OK)C(!*output);
    xr_xir_compile_c_source_free(&source);xr_xir_compile_artifact_free(checked);
    xr_xir_compile_artifact_free(special);xr_xir_compile_artifact_free(lowered);return status;
}
int main(int argc,char **argv) {
    C(argc==1 || argc==2);FILE *file=fopen(XR_CHECKED_FIXTURE,"rb");C(file);
    C(!fseek(file,0,SEEK_END));long size=ftell(file);C(size>=64 && size<=262144);C(!fseek(file,0,SEEK_SET));
    uint8_t *bytes=malloc((size_t)size);C(bytes);C(fread(bytes,1,(size_t)size,file)==(size_t)size);C(!fclose(file));
    class_input_bytes=bytes;class_input_length=(size_t)size;class_c_path=argc==2?argv[1]:NULL;
    XrXirCompileContext context=class_array_context(class_array_limits());XrCompileResourceStats baseline=class_array_stats(&context);
    class_array_contract_packet(&context,bytes,(size_t)size);XrXirProgram *program=NULL;
    C(class_packet_build(&context,0,&program)==XR_XIR_OK);class_array_pipeline_report(&context,"packet VM");
 class_runtime_faults(program);
 XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
 C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
 C(xr_xir_instance_start(instance,3,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64 && number.payload==41);xr_xir_value_drop(&number);
 C(xr_xir_instance_start(instance,4,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
 C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 class_array_suspend_cases(program);xr_xir_compile_program_drop(program);
 for(unsigned i=0;i<2;++i)class_array_retained(&saved[i]);
 C(!runtime_live && !runtime_bytes);
    class_array_owner_free(&context,baseline);class_c_path=NULL;
    if(argc==1)class_array_compiler_faults(class_packet_build,0);
    memset(bytes,0xCC,(size_t)size);free(bytes);puts("source-free packet VM class41 retained Array/string physical release PASS");return 0;
}
