/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_typed_await_native.c - Actual generated callbacks retain typed cancellation owners
 */
#include "xir/xxir_program_internal.h"
#include "xir/xxir_output.h"
#include "xir/xxir_task.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_error.h"
#include "xir/xxir_enum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"native typed await %d: %s\n",__LINE__,#c);exit(1);}} while (0)
extern const XrXirProgramSpec source_artifact_program;
typedef struct Output {char bytes[4096];size_t length;} Output;
static XrXirOutputStatus capture(void *context,XrXirOutputStream stream,const char *bytes,size_t length) {
    Output *output=(Output *)context;CHECK(stream==XR_XIR_STDOUT && length<=sizeof(output->bytes)-output->length);
    memcpy(output->bytes+output->length,bytes,length);output->length+=length;return XR_XIR_OUTPUT_OK;
}
static uint32_t named(const XrXirProgram *program,const char *name) {
    const XrXirModule *module=xr_xir_compile_artifact_module(program->permissions->input->artifact);
    for (uint32_t i=0;i<module->function_count;++i)
        if (module->functions[i].name_length==strlen(name) && !memcmp(module->functions[i].name,name,strlen(name))) return i;
    CHECK(false);return UINT32_MAX;
}

static XrXirValue run(XrXirInstance *instance,uint32_t entry,XrXirCallStatus expected) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==expected);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==expected);return result;
}
static void late_enum(const XrXirValue *error,const char *name,bool payload) {
    XrXirValue concrete=*error;
    if (error->type==XR_XIR_ERROR) CHECK(xr_xir_error_borrow(error,&concrete));
    if (!payload) {
        const XrXirTypes *types=xr_xir_compile_type_arena_types(xr_xir_value_arena(error));
        CHECK(xr_xir_cancelled_error_type(types)==(XrXirType)concrete.type);
    }
    XrXirEnumBorrow value={0};CHECK(xr_xir_enum_borrow(&concrete,&value)==XR_XIR_VALUE_OK);
    CHECK(value.name.length==strlen(name) && !memcmp(value.name.bytes,name,value.name.length));
    CHECK(value.field_count==(payload?1u:0u));
    if (payload) {
        const char expected[]="owned \303\251\0\347\225\214";const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_string_view(&value.fields[0],&bytes,&length));
        CHECK(length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    } else CHECK(value.member.length==9 && !memcmp(value.member.bytes,"Cancelled",9));
}
int main(void) {
    XrCompileResourceLimits limits={0};limits.memory_mode=XR_COMPILE_MEMORY_ORDINARY;
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_program_seal(&context,&source_artifact_program,&program)==XR_XIR_OK);
    xr_compile_resources_release(context.resources);
    CHECK(xr_xir_cancelled_error_type(source_artifact_program.types)!=XR_XIR_UNIT);
    XrXirValue untouched={0};CHECK(xr_xir_task_cancel(NULL,&untouched)==XR_XIR_CALL_BAD_ARGUMENT && !untouched.type && !untouched.payload);
    Output output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,capture,&output,sizeof(output.bytes)};
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    uint32_t never=named(program,"awaitNever"),one=named(program,"awaitOne"),cancelled_entry=named(program,"cancelledError");
    xr_xir_compile_program_drop(program);
    XrXirValue initialized=run(instance,source_artifact_program.declarations->entry_function,XR_XIR_CALL_RETURNED);
    xr_xir_value_drop(&initialized);
    static const char expected[]="child-cleanup\n2\nowned \303\251\0\347\225\214\none-error\nunion-a\nunion-b\n";
    CHECK(output.length==sizeof(expected)-1 && !memcmp(output.bytes,expected,output.length));output.length=0;
    XrXirValue owned=run(instance,never,XR_XIR_CALL_RETURNED);
    XrXirValue error=run(instance,one,XR_XIR_CALL_THROWN);
    XrXirValue cancelled=run(instance,cancelled_entry,XR_XIR_CALL_THROWN);
    CHECK(output.length==14 && !memcmp(output.bytes,"child-cleanup\n",14));
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char bytes_expected[]="owned \303\251\0\347\225\214";const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&owned,&bytes,&length) && length==sizeof(bytes_expected)-1 && !memcmp(bytes,bytes_expected,length));
    late_enum(&error,"A",true);late_enum(&cancelled,"Cancelled",false);
    xr_xir_value_drop(&owned);xr_xir_value_drop(&error);xr_xir_value_drop(&cancelled);
    puts("generated C11 typed await: cleanup ordering, sticky outcomes and retired UTF8/NUL/Error owners PASS");return 0;
}
