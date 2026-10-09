/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Independent literal result on actual Program and two separately owned Instances. */
#ifndef XIR_PRELUDE_RUNTIME_H
#define XIR_PRELUDE_RUNTIME_H
#include "xir/xxir.h"
XR_FUNC void xr_test_prelude_run(const XrXirCompileContext *context,XrXirArtifact *owned);
XR_FUNC XrXirStatus xr_test_prelude_compile_pipeline(const XrXirCompileContext *context,const XrXirArtifact *checked);
#ifdef XR_PRELUDE_RUNTIME_IMPLEMENTATION
#include "xir/xxir_output.h"
typedef struct PreludeOutput { size_t bytes; } PreludeOutput;
static XrXirOutputStatus prelude_output(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    PreludeOutput *output=context;(void)stream;(void)bytes;output->bytes+=length;return XR_XIR_OUTPUT_OK;
}
static bool prelude_pair(XrXirProgram *program,uint32_t answer) {
    XrXirInstance *instances[2]={0};PreludeOutput output[2]={{0}};XrXirOutputSink sinks[2]={{0}};bool ok=false;
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        sinks[i]=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,prelude_output,&output[i],64};
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sinks[i]};
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instances[i]);
        if (status==XR_XIR_CALL_OOM) goto done;
        CHECK(status==XR_XIR_CALL_READY);
    }
    for (unsigned repeat=0;repeat<2;++repeat) for (unsigned i=0;i<2;++i) {
        uint64_t peer=instances[1-i]->epoch;
        XrXirCallStatus status=xr_xir_instance_start(instances[i],answer,NULL,0);
        if (status==XR_XIR_CALL_READY) status=xr_xir_instance_poll_bounded(instances[i],UINT64_MAX).outcome.status;
        if (status==XR_XIR_CALL_OOM) goto done;
        CHECK(status==XR_XIR_CALL_RETURNED);
        XrXirValue result={0};status=xr_xir_instance_take_result(instances[i],&result);
        if (status==XR_XIR_CALL_OOM) {CHECK(!result.type);goto done;}
        CHECK(status==XR_XIR_CALL_RETURNED && result.type==XR_XIR_I64 && result.payload==42);
        xr_xir_value_drop(&result);CHECK(!output[i].bytes && !output[1-i].bytes);
        CHECK(instances[1-i]->epoch==peer);
    }
    ok=true;
done:
    for (unsigned i=0;i<2;++i) if(instances[i]) CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    return ok;
}
static XrXirStatus prelude_seal_operation(const XrXirCompileContext *context,void *opaque) {
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_program_seal(context,opaque,&program);
    CHECK(status==XR_XIR_OK ? program!=NULL : program==NULL);xr_xir_compile_program_drop(program);return status;
}
static void prelude_finish(const XrXirCompileContext *context,XrXirProgramSpec *spec,XrXirArtifact *lowered,uint32_t answer) {
    library_compile_operation_cases("Canonical prelude actual Program seal",prelude_seal_operation,spec);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(context,spec,&program)==XR_XIR_OK);
    size_t live=runtime_live,bytes=runtime_bytes,sites=0;
    for (size_t p=0;p<=sites;++p) {
        runtime_attempts=0;runtime_fail_at=p ? p-1 : SIZE_MAX;
        bool ok=prelude_pair(program,answer);
        if (!p) {CHECK(ok);sites=runtime_attempts;CHECK(sites);} else CHECK(!ok);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    runtime_fail_at=SIZE_MAX;CHECK(prelude_pair(program,answer));
    xr_xir_compile_program_drop(program);xr_xir_compile_artifact_free(lowered);
    CHECK(!runtime_live && !runtime_bytes);
    printf("Canonical prelude fixed42 twoInstances repeatedCalls actualRuntimeOOM=%zu physicalzero\n",sites);
}
#endif
#endif
