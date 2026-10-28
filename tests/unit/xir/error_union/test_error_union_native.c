/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_error_union_native.c - Execute complete Error metadata as portable C11
 */
#include "xir/xxir_program.h"
#include "xir/xxir_output.h"
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
extern const XrXirProgramSpec error_union_program;
extern const uint32_t error_union_escaped,error_union_answer;
typedef struct Output {char bytes[128];size_t length;bool fail;} Output;
static XrXirOutputStatus output_bytes(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    Output *out=context;CHECK(stream==XR_XIR_STDOUT && length<=sizeof(out->bytes)-out->length);
    if (out->fail) return XR_XIR_OUTPUT_ERROR;
    memcpy(out->bytes+out->length,bytes,length);out->length+=length;return XR_XIR_OUTPUT_OK;
}
static XrXirValue run_entry(XrXirInstance *instance,uint32_t entry,XrXirCallStatus expected) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==expected);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==expected);return result;
}
static void late_error(XrXirValue *error) {
    const char expected[]="owned é\0界";XrXirValue concrete=*error;XrXirEnumBorrow value={0};
    if (error->type==XR_XIR_ERROR) CHECK(xr_xir_error_borrow(error,&concrete));
    CHECK(xr_xir_enum_borrow(&concrete,&value)==XR_XIR_VALUE_OK);
    CHECK(value.name.length==5 && !memcmp(value.name.bytes,"First",5));
    CHECK(value.member.length==3 && !memcmp(value.member.bytes,"Bad",3) && value.field_count==1);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&value.fields[0],&bytes,&length));
    CHECK(length==sizeof(expected)-1 && !memcmp(bytes,expected,length));xr_xir_value_drop(error);
}
static void native_case(bool output_failure) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&error_union_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    Output output={.fail=output_failure};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,output_bytes,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);uint32_t entry=error_union_program.declarations->entry_function;
    if (output_failure) {
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_OUTPUT_ERROR && !output.length);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_OUTPUT_ERROR);return;
    }
    for (unsigned i=0;i<2;++i) {XrXirValue value=run_entry(instance,entry,XR_XIR_CALL_RETURNED);xr_xir_value_drop(&value);}
    CHECK(output.length==3 && !memcmp(output.bytes,"42\n",3));
    XrXirValue answer=run_entry(instance,error_union_answer,XR_XIR_CALL_RETURNED);
    CHECK(answer.type==XR_XIR_I64 && answer.payload==42);xr_xir_value_drop(&answer);XrXirValue error={0};
    if (error_union_escaped!=UINT32_MAX) error=run_entry(instance,error_union_escaped,XR_XIR_CALL_THROWN);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    if (error_union_escaped!=UINT32_MAX) late_error(&error);
}
int main(void) {
    native_case(false);native_case(true);
    puts("Portable C11: complete Error metadata, repeated entry, late UTF8/NUL owner and output failure PASS");return 0;
}
