/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_prelude_runtime.h - Actual initialization effects under one sealed Program
 *
 * KEY CONCEPT:
 *   Only explicit imports activate the Path initializer; type visibility does not.
 */
#ifndef XIR_PATH_PRELUDE_RUNTIME_H
#define XIR_PATH_PRELUDE_RUNTIME_H
#include "xir/xxir_call.h"
#include "xir/xxir_program.h"
#include "xir/xxir_value.h"
#include "xir/xxir_output.h"
static unsigned path_markers;
static XrXirOutputStatus path_output(void *unused,const XrXirOutputGroup *group) {
    (void)unused;const char *text=NULL;size_t length=0;
    if(!group||group->stream!=XR_XIR_STDOUT||!group->line||group->count!=1||
        !xr_xir_string_view(group->values,&text,&length)||length!=16||memcmp(text,"Path initializer",16))return XR_XIR_OUTPUT_ERROR;
    ++path_markers;return XR_XIR_OUTPUT_OK;
}
static void path_execute(XrXirProgram *program,uint32_t entry,unsigned expected) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,path_output,NULL};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    path_markers=0;CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,16).outcome.status;
    }
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED&&value.type==XR_XIR_I64&&value.payload==42);
    xr_xir_value_drop(&value);CHECK(path_markers==expected);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
#endif // XIR_PATH_PRELUDE_RUNTIME_H
