/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native.c - Independently expected generated test and hook execution
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
extern const XrXirProgramSpec test_roles_program;
extern const uint32_t test_role_functions[7];
typedef struct Output { char bytes[256]; size_t length; XrXirOutputSink sink; } Output;
static XrXirOutputStatus bytes(void *context,XrXirOutputStream stream,const char *text,size_t length) {
    Output *out=context;CHECK(stream==XR_XIR_STDOUT && length<=sizeof(out->bytes)-out->length);
    memcpy(out->bytes+out->length,text,length);out->length+=length;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus group(void *context,const XrXirOutputGroup *out) {
    return xr_xir_output_render(&((Output *)context)->sink,out);
}
static XrXirCallStatus finish_test(XrXirInstance *instance) {
    XrXirInstanceResult result;
    unsigned slices=0;
    do {
        CHECK(++slices<=1024);
        result=xr_xir_instance_poll_bounded(instance,1);
    } while(result.outcome.status==XR_XIR_CALL_READY);
    return result.outcome.status;
}
int main(void) {
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&test_roles_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    Output out={0};out.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,bytes,&out,sizeof(out.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,group,&out};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    const uint32_t roles[]={3,5,1,2,1,6,4};
    for(uint32_t i=0;i<7;++i) {
        const XrXirFunctionIdentity *identity=&test_roles_program.declarations->functions[test_role_functions[i]];
        CHECK(identity->test_role==roles[i] && identity->test_timeout_seconds==(i==4?7u:0u));
        CHECK(xr_xir_instance_start(instance,test_role_functions[i],NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
        if(i==3) {CHECK(xr_xir_instance_start_test(instance,test_role_functions[i])==XR_XIR_CALL_BAD_ARGUMENT);continue;}
        CHECK(xr_xir_instance_start_test(instance,test_role_functions[i])==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start_test(instance,test_role_functions[i])==XR_XIR_CALL_BUSY);
        CHECK(finish_test(instance)==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_UNIT);xr_xir_value_drop(&value);
    }
    size_t before_cancel=out.length;
    CHECK(xr_xir_instance_start_test(instance,test_role_functions[2])==XR_XIR_CALL_READY);
    for(unsigned slices=0;out.length==before_cancel;++slices) {
        CHECK(slices<1024);
        CHECK(xr_xir_instance_poll_bounded(instance,1).outcome.status==XR_XIR_CALL_READY);
    }
    CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
    CHECK(xr_xir_instance_start_test(instance,test_role_functions[5])==XR_XIR_CALL_BUSY);
    CHECK(finish_test(instance)==XR_XIR_CALL_CANCELLED);
    XrXirValue cancelled={0};
    CHECK(xr_xir_instance_take_result(instance,&cancelled)==XR_XIR_CALL_BAD_STATE);
    CHECK(!cancelled.type && !cancelled.reserved && !cancelled.payload);
    const uint32_t after_cancel[]={test_role_functions[5],test_role_functions[4]};
    for(unsigned i=0;i<2;++i) {
        CHECK(xr_xir_instance_start_test(instance,after_cancel[i])==XR_XIR_CALL_READY);
        CHECK(finish_test(instance)==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        CHECK(value.type==XR_XIR_UNIT);xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char expected[]="init\nbefore_all\nbefore_each\n1\n2\nafter_each\nafter_all\n3\nafter_each\n4\n";
    CHECK(out.length==sizeof(expected)-1 && !memcmp(out.bytes,expected,sizeof(expected)-1));
    puts("native independent hook/test output oracle PASS");return 0;
}
