/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_from_utf8_native.c - Execute Source-created portable C output
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#define FROM_UTF8_EXPECTED_ONLY
#include "from_utf8_cases.h"
extern const XrXirProgramSpec source_artifact_program;
typedef struct Output { char bytes[4096];size_t size;XrXirOutputSink sink; } Output;
static XrXirOutputStatus bytes(void *context,XrXirOutputStream stream,const char *text,size_t size) {
    Output *out=context;CHECK(stream==XR_XIR_STDOUT && size<=sizeof(out->bytes)-out->size);
    memcpy(out->bytes+out->size,text,size);out->size+=size;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus group(void *context,const XrXirOutputGroup *value) {
    return xr_xir_output_render(&((Output *)context)->sink,value);
}
int main(int argc,char **argv) {
    bool bad_state=argc==2 && !strcmp(argv[1],"--expect-bad-state");
    CHECK(argc==1 || bad_state || (argc==3 && !strcmp(argv[1],"--expect")));
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
    XrXirCallStatus terminal=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    CHECK(terminal==(bad_state?XR_XIR_CALL_BAD_STATE:XR_XIR_CALL_RETURNED));
    if (bad_state) {
        CHECK(!output.size);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_BAD_STATE);
        puts("Generated C rejects non-Sendable panic Error before Task terminal publication");return 0;
    }
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && xr_xir_value_valid(&result));xr_xir_value_drop(&result);
    if (argc==3) {
        FILE *file=fopen(argv[2],"rb");CHECK(file && !fseek(file,0,SEEK_END));long length=ftell(file);
        CHECK(length>=0 && (size_t)length==output.size && !fseek(file,0,SEEK_SET));
        char expected[sizeof(output.bytes)];CHECK(fread(expected,1,output.size,file)==output.size && !fclose(file));
        CHECK(!memcmp(expected,output.bytes,output.size));
    } else CHECK(output.size==sizeof(from_utf8_expected)-1 && !memcmp(output.bytes,from_utf8_expected,output.size));
    puts("Strict fromUtf8 generated C: owned Unicode/NUL and typed catch PASS");return 0;
}
