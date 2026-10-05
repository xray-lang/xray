/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_class_source.c - Real class identity execution and lifetime gates
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
#include "xir_class_pipeline_owner.h"
#include "xir_runtime_allocations.h"
#define SOURCE_CLASS_FUNCTIONS 11
#define LOWERED_CLASS_FUNCTIONS 11

static XrXirStatus class_source_owned(const XrXirCompileContext *context,XrXirArtifact **output) {
    XrCompilerSession *session=NULL;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrCompilerSessionStatus made=xr_compile_session_new(context->resources,&session);
    XrXirStatus status=made==XR_COMPILER_SESSION_OK?XR_XIR_OK:made==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_SOURCE_FIXTURES};
    XrXirSourceRequest request={session,XR_SOURCE_FIXTURES "/root.xr",&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    if(status==XR_XIR_OK){
        status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
        if(status!=XR_XIR_OK){CHECK(diagnostic.status==status && !result.checked);
            if(result.snapshot)CHECK(!xr_xir_compile_source_snapshot_view(result.snapshot)->complete);}
    }
    if(status==XR_XIR_OK){
        CHECK(result.checked && result.snapshot && xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
        CHECK(xr_xir_compile_artifact_module(result.checked)->function_count==SOURCE_CLASS_FUNCTIONS);
        *output=result.checked;result.checked=NULL;
    }
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(*output,NULL);
    if(status!=XR_XIR_OK){xr_xir_compile_artifact_free(*output);*output=NULL;}
    return status;
}
static XrXirStatus class_source_program(const XrXirCompileContext *context,XrXirProgram **output,const char *path) {
    XrXirArtifact *owned=NULL,*decoded=NULL,*special=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=class_source_owned(context,&owned);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_write(owned,&packet,NULL);
    if(status==XR_XIR_OK){CHECK(packet.length<=262144);if(path){FILE *f=fopen(path,"wb");CHECK(f && fwrite(packet.bytes,1,packet.length,f)==packet.length && !fclose(f));}}
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL);
    xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(owned);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(decoded,&special,NULL);
    xr_xir_compile_artifact_free(decoded);
    if(status==XR_XIR_OK)status=xr_xir_compile_artifact_verify(special,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(special,&target,&lowered,NULL);
    xr_xir_compile_artifact_free(special);
    if(status==XR_XIR_OK){const XrXirModule *m=xr_xir_compile_artifact_module(lowered);
        CHECK(m->function_count==LOWERED_CLASS_FUNCTIONS && m->declarations->module_count==3);
        CHECK(m->functions[3].name_length==6 && !memcmp(m->functions[3].name,"answer",6));
        CHECK(m->functions[4].name_length==8 && !memcmp(m->functions[4].name,"retained",8));
        status=xr_xir_compile_vm_program_take(&lowered,output);
    }
    xr_xir_compile_artifact_free(lowered);if(status!=XR_XIR_OK)CHECK(!*output);return status;
}
static XrXirStatus class_source_operation(const XrXirCompileContext *context,void *fixture) {
    (void)fixture;XrXirProgram *program=NULL;XrXirStatus status=class_source_program(context,&program,NULL);
    xr_xir_compile_program_drop(program);return status;
}
int main(int argc,char **argv){
 C(argc==1 || argc==2);ClassPipelineOwner owner={0};C(class_pipeline_new(&owner,&class_pipeline_limits)==XR_XIR_OK);
 XrXirProgram *program=NULL;C(class_source_program(&owner.context,&program,argc==2?argv[1]:NULL)==XR_XIR_OK);
 const uint32_t answer=3,retained=4;XrXirValue saved[2]={{0},{0}};
 for(unsigned i=0;i<2;++i){XrXirInstance *instance=NULL;XrXirInstanceConfig config; C(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);C(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);C(xr_xir_instance_start(instance,answer,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);XrXirValue number={0};C(xr_xir_instance_take_result(instance,&number)==XR_XIR_CALL_RETURNED);C(number.type==XR_XIR_I64&&number.payload==41);xr_xir_value_drop(&number);C(xr_xir_instance_start(instance,retained,NULL,0)==XR_XIR_CALL_READY);C(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);C(xr_xir_instance_take_result(instance,&saved[i])==XR_XIR_CALL_RETURNED);C(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);}
 xr_xir_compile_program_drop(program);for(unsigned i=0;i<2;++i){XrXirValue value={0};C(xr_xir_class_get(&saved[i],0,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&saved[i]),.work=10000},&value)==XR_XIR_VALUE_OK);C(value.type==XR_XIR_I64&&value.payload==40);xr_xir_value_drop(&value);C(xr_xir_class_get(&saved[i],1,&(XrXirValueAdmission){.arena=xr_xir_value_arena(&saved[i]),.work=10000},&value)==XR_XIR_VALUE_OK);const char *bytes=NULL;size_t length=0;C(xr_xir_string_view(&value,&bytes,&length)&&length==7&&!memcmp(bytes,"counter",7));xr_xir_value_drop(&saved[i]);C(xr_xir_string_view(&value,&bytes,&length)&&length==7&&!memcmp(bytes,"counter",7));xr_xir_value_drop(&value);}puts("source class VM41 retained PASS");C(!runtime_live && !runtime_bytes);class_pipeline_drop(&owner);class_pipeline_faults("Class identity whole Source/Checked/VM",class_source_operation,NULL);class_pipeline_final_zero();return 0;}
