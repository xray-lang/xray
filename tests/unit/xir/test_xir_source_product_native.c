/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_product_native.c - Native source program with a fixed output oracle
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
extern const XrXirProgramSpec linked_source_program;
typedef struct Output { char bytes[64]; size_t size; XrXirOutputSink sink; } Output;
static XrXirOutputStatus bytes(void *context, XrXirOutputStream stream, const char *text, size_t size) {
    Output *output=context;
    CHECK(stream==XR_XIR_STDOUT && size<=sizeof(output->bytes)-output->size);
    memcpy(output->bytes+output->size,text,size); output->size+=size;
    return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus group(void *context, const XrXirOutputGroup *value) {
    return xr_xir_output_render(&((Output *)context)->sink,value);
}
int main(void) {
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_program_seal(&context,&linked_source_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    Output output={0}; output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,group,&output};
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,linked_source_program.declarations->entry_function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};
    CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && !result.reserved && !result.payload);
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char expected[]="owner\xe4\xb8\xad-ok 42 true\n";
    CHECK(output.size==sizeof(expected)-1 && !memcmp(output.bytes,expected,sizeof(expected)-1));
    puts("native source program: independent module state, UTF-8 and typed output oracle passed");
    return 0;
}
