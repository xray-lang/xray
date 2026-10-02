/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_execution_gates.h - Shared original-fixture VM/native resource gates
 */
#ifndef SOURCE_EXECUTION_GATES_H
#define SOURCE_EXECUTION_GATES_H
typedef struct ProbeOutput { char bytes[128]; size_t length, calls; bool reject, render_oom; XrXirOutputSink sink; } ProbeOutput;
typedef struct ProbeRun { size_t allocations; XrXirCallStatus status; bool render_oom; } ProbeRun;
static XrXirOutputStatus probe_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    ProbeOutput *output=context;
    CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    ++output->calls;if (output->reject) return XR_XIR_OUTPUT_ERROR;
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus probe_render(void *context,const XrXirOutputGroup *group) {
    ProbeOutput *output=context;size_t before=runtime_attempts;
    XrXirOutputStatus status=xr_xir_output_render(&output->sink,group);
    output->render_oom=runtime_fail_at>=before && runtime_fail_at<runtime_attempts;
    return status;
}
static ProbeRun probe_once(XrXirProgram *program,uint32_t entry,const char *expected,
                           uint64_t polls,size_t fail_at,unsigned mode) {
    size_t live=runtime_live,bytes=runtime_bytes;ProbeOutput output={0};output.reject=mode==1;
    output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,probe_bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);config.poll_limit=polls;
    config.output=(XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, probe_render, &output};
    runtime_attempts=0;runtime_fail_at=fail_at;XrXirInstance *instance=NULL;
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
    if (status==XR_XIR_CALL_READY) {
        status=xr_xir_instance_start(instance,entry,NULL,0);
        if (status==XR_XIR_CALL_READY) {
            if (mode==2) CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
            XrXirInstanceResult polled=xr_xir_instance_poll(instance);status=polled.outcome.status;
            CHECK(xr_xir_call_result_valid(&polled.outcome));
            if (status==XR_XIR_CALL_RETURNED) {
                XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==status);
                CHECK(value.type==XR_XIR_I64 && !value.reserved && !value.payload);xr_xir_value_drop(&value);
            } else {
                CHECK(!xr_xir_call_panic_status(status));
                CHECK(polled.outcome.value.type==XR_XIR_UNIT && !polled.outcome.value.payload);
                if (xr_xir_instance_state(instance)==XR_XIR_INSTANCE_FAILED) {
                    XrXirCallResult copied={0};CHECK(xr_xir_instance_copy_failure(instance,&copied)==status);
                    CHECK(xr_xir_call_result_valid(&copied));xr_xir_call_result_drop(&copied);
                }
            }
        }
    }
    ProbeRun result={runtime_attempts,status,output.render_oom};runtime_fail_at=SIZE_MAX;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    size_t length=strlen(expected);CHECK(output.length==0 || output.length==length);
    CHECK(!memcmp(output.bytes,expected,output.length));
    if (status==XR_XIR_CALL_RETURNED) CHECK(output.length==length);
    return result;
}
static void probe_program(XrXirProgram *program,uint32_t entry,const char *expected) {
    ProbeRun normal=probe_once(program,entry,expected,64000000,SIZE_MAX,0);
    CHECK(normal.status==XR_XIR_CALL_RETURNED && normal.allocations);
    size_t renderer_sites=0;
    for (size_t i=0;i<normal.allocations;++i) {
        ProbeRun failure=probe_once(program,entry,expected,64000000,i,0);
        CHECK(failure.status==XR_XIR_CALL_OOM);
        renderer_sites+=failure.render_oom ? 1 : 0;
    }
    uint64_t low=0,high=64000000;
    while (high-low>1) {
        uint64_t middle=low+(high-low)/2;
        ProbeRun bounded=probe_once(program,entry,expected,middle,SIZE_MAX,0);
        CHECK(bounded.status==XR_XIR_CALL_RETURNED || bounded.status==XR_XIR_CALL_LIMIT);
        if (bounded.status==XR_XIR_CALL_RETURNED) high=middle;else low=middle;
    }
    CHECK(probe_once(program,entry,expected,high,SIZE_MAX,0).status==XR_XIR_CALL_RETURNED);
    CHECK(low && probe_once(program,entry,expected,low,SIZE_MAX,0).status==XR_XIR_CALL_LIMIT);
    if (*expected) CHECK(probe_once(program,entry,expected,64000000,SIZE_MAX,1).status==XR_XIR_CALL_OUTPUT_ERROR);
    CHECK(probe_once(program,entry,expected,64000000,SIZE_MAX,2).status==XR_XIR_CALL_CANCELLED);
    printf("original entry: allocations=%zu renderer_typed_oom=%zu poll_exact=%llu minus1=%llu resource/physical PASS\n",
           normal.allocations,renderer_sites,(unsigned long long)high,(unsigned long long)low);
}
#endif // SOURCE_EXECUTION_GATES_H
