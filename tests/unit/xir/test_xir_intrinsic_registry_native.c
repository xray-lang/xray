/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_intrinsic_registry_native.c - Execute the real emitted call program
 *
 * KEY CONCEPT:
 *   The emitted program uses the existing instance ABI and production helper.
 *   A fixed output oracle observes calls made after Source owners have died.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
extern const XrXirProgramSpec intrinsic_contains_program;
typedef struct Output { char bytes[128]; size_t size; XrXirOutputSink sink; } Output;
static XrXirOutputStatus bytes(void *context, XrXirOutputStream stream, const char *text, size_t size) {
    Output *output = context;
    CHECK(stream == XR_XIR_STDOUT && size <= sizeof(output->bytes) - output->size);
    memcpy(output->bytes + output->size, text, size);
    output->size += size;
    return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus group(void *context, const XrXirOutputGroup *value) {
    return xr_xir_output_render(&((Output *)context)->sink, value);
}
static void native_slice_ranges(void) {
    const XrXirProgramSpec *spec=&intrinsic_contains_program;uint32_t function=UINT32_MAX;
    for(uint32_t i=0;i<spec->entry_count;++i) {
        const XrXirCallEntry *entry=spec->entries+i;
        if(entry->parameter_count==3 && entry->result==XR_XIR_STRING && spec->declarations->functions[i].exported &&
            entry->parameters[0]==XR_XIR_STRING && entry->parameters[1]==XR_XIR_I64 && entry->parameters[2]==XR_XIR_I64) {
            CHECK(function==UINT32_MAX);function=i;
        }
    }
    CHECK(function!=UINT32_MAX);
    const int64_t ranges[][2]={{1,3},{0,4},{4,4},{-1,0},{2,1},{0,5},{0,INT64_MAX}};
    const char *expected[]={"\xc3\xa9\xe4\xb8\xad","A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80",""};
    for(size_t i=0;i<sizeof(ranges)/sizeof(*ranges);++i) {
        XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,false};
        XrXirCompileContext context={NULL,xr_xir_compile_default_limits()};
        CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
        XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,spec,&program)==XR_XIR_OK);
        xr_compile_resources_release(context.resources);
        Output output={0};output.sink=(XrXirOutputSink){XR_XIR_CALL_ABI_VERSION,0,bytes,&output,sizeof(output.bytes)};
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,group,&output};
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        xr_xir_compile_program_drop(program);
        XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
        XrXirValue arguments[3]={{0},{XR_XIR_I64,0,ranges[i][0]},{XR_XIR_I64,0,ranges[i][1]}};
        const char text[]="A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80";
        CHECK(xr_xir_string_new(domain,text,sizeof(text)-1,arguments)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_instance_start(instance,function,arguments,3)==XR_XIR_CALL_READY);
        xr_xir_value_drop(arguments);xr_xir_domain_drop(domain);
        XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        CHECK(polled.outcome.status==(i<3?XR_XIR_CALL_RETURNED:XR_XIR_CALL_RUNTIME_PANIC));
        XrXirValue result={0};
        if(i<3) CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
        else {
            CHECK(polled.outcome.panic.detail.code==XR_XIR_PANIC_STRING_SLICE_RANGE && !polled.outcome.panic.message.type);
            char message[96];const char expected_message[]="string slice range is invalid";
            CHECK(xr_xir_panic_message_format(polled.outcome.panic.detail,message,sizeof(message))==sizeof(expected_message)-1 &&
                !strcmp(message,expected_message));
        }
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
        if(i<3) {
            const char *text_bytes=NULL;size_t size=0;CHECK(xr_xir_string_view(&result,&text_bytes,&size));
            CHECK(size==strlen(expected[i]) && !memcmp(text_bytes,expected[i],size));
        } else CHECK(!result.type && !result.payload);
        xr_xir_value_drop(&result);
    }
}
int main(void) {
    XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_ORDINARY, 0, false};
    XrXirCompileContext context = {NULL, xr_xir_compile_default_limits()};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &intrinsic_contains_program, &program) == XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    Output output = {0};
    output.sink = (XrXirOutputSink) {XR_XIR_CALL_ABI_VERSION, 0, bytes, &output, sizeof(output.bytes)};
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, group, &output};
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance, intrinsic_contains_program.declarations->entry_function,
        NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_value_valid(&result));
    xr_xir_value_drop(&result);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    const char expected[] = "true false true\ntrue true false false true false\n"
        "\xc3\xa9\xe4\xb8\xad bc 1 \xc3\xa9\xe4\xb8\xad 42 42 \xc3\xa9\xe4\xb8\xad:changed\n";
    CHECK(output.size == sizeof(expected) - 1 && !memcmp(output.bytes, expected, sizeof(expected) - 1));
    native_slice_ranges();
    puts("generated intrinsic program: contains, prefix and suffix fixed oracles passed");
    return 0;
}
