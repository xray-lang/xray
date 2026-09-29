/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_effects_native.c - Qualified callback execution without the compiler or VM
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec effect_source_program;
XR_DATA const uint32_t effect_source_entry;
static void native_attempt(XrXirProgram *program) {
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    XrXirInstance *instance = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, effect_source_entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll(instance).outcome.status;
    XrXirValue value = {0};
    if (status == XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == 7);
    } else CHECK(runtime_fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&value);
}
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&effect_source_program,
        (XrXirProgramBudget){33554432, 64000000}, &program) == XR_XIR_OK);
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        native_attempt(program);
        if (!attempt) sites = runtime_attempts;
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX; xr_xir_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
    printf("Native qualified callback released %zu allocation failure sites\n", sites);
    puts("Native qualified callback returned the independent expected value 7");
    return 0;
}
