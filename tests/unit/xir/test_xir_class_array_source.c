/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_array_source.c - Real class identity execution and lifetime gates
 *
 * KEY CONCEPT:
 *   One Checked source graph feeds independent VM and native expected results.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_class.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define C(x) do{if(!(x)){fprintf(stderr,"FAIL %d %s\n",__LINE__,#x);exit(1);}}while(0)
#define CHECK(x) C(x)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_class_array_compile_owner.h"
#include "xir_class_array_graph_cases.h"
#include "xir_class_array_retained.h"
static const char *class_packet_path;
static XrXirStatus class_source_build(const XrXirCompileContext *context,unsigned variant,XrXirProgram **output) {
    (void)variant;XrCompilerSession *session=NULL;XrXirSourceResult result={0};
    XrXirArtifact *checked=NULL,*special=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrCompilerSessionStatus made=xr_compile_session_new(context->resources,&session);
    XrXirStatus status=made==XR_COMPILER_SESSION_OK?XR_XIR_OK:made==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceDiagnostic diagnostic={0};
    if(status==XR_XIR_OK)status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status==XR_XIR_OK){checked=result.checked;result.checked=NULL;}
    else C(!result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(checked,&packet,NULL);
    if(status==XR_XIR_OK){C(packet.length<=262144);if(class_packet_path){FILE *file=fopen(class_packet_path,"wb");C(file);C(fwrite(packet.bytes,1,packet.length,file)==packet.length);C(!fclose(file));}}
    if(status==XR_XIR_OK){xr_xir_compile_artifact_free(checked);checked=NULL;
        status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL);}
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&special,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(special,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    if(status==XR_XIR_OK)class_array_graph_verify(xr_xir_compile_artifact_module(lowered));
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,output);
    if(status!=XR_XIR_OK)C(!*output);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(checked);
    xr_xir_compile_artifact_free(special);xr_xir_compile_artifact_free(lowered);return status;
}
int main(int argc,char **argv){
    C(argc==1 || argc==2);class_packet_path=argc==2?argv[1]:NULL;
    XrXirCompileContext context=class_array_context(class_array_limits());XrCompileResourceStats baseline=class_array_stats(&context);
    XrXirProgram *program=NULL;C(class_source_build(&context,0,&program)==XR_XIR_OK);
    class_array_pipeline_report(&context,"Source");const uint32_t answer=CLASS_ARRAY_ANSWER,retained=CLASS_ARRAY_RETAINED;
XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);C(xr_xir_instance_start(instance,answer,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64&&number.payload==41);xr_xir_value_drop(&number);C(xr_xir_instance_start(instance,retained,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 class_array_suspend_cases(program);xr_xir_compile_program_drop(program);for(unsigned i=0;i<2;++i)class_array_retained(&saved[i]);puts("source class Array VM41 retained PASS");class_array_owner_free(&context,baseline);class_packet_path=NULL;
 if(argc==1)class_array_compiler_faults(class_source_build,0);return 0;}
