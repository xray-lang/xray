/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_nullable_print_native.c - Execute Source-created portable C output
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "nullable_string_print_expected.h"
extern const XrXirProgramSpec source_artifact_program;
typedef struct Output { char bytes[4096];size_t size;XrXirOutputSink sink; } Output;
static XrXirOutputStatus bytes(void *context,XrXirOutputStream stream,const char *text,size_t size) {
    Output *out=context;CHECK(stream==XR_XIR_STDOUT && size<=sizeof(out->bytes)-out->size);
    memcpy(out->bytes+out->size,text,size);out->size+=size;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus group(void *context,const XrXirOutputGroup *value) {
    return xr_xir_output_render(&((Output *)context)->sink,value);
}
int main(void) {
    XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&source_artifact_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    Output output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,group,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,source_artifact_program.declarations->entry_function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && xr_xir_value_valid(&result));xr_xir_value_drop(&result);
    CHECK(output.size==sizeof(nullable_print_expected)-1 && !memcmp(output.bytes,nullable_print_expected,output.size));
    puts("Source nullable String print generated C: byte-exact Unicode/NUL/snapshot output PASS");return 0;
}
