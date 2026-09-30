/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_package_native.c - Runtime-only locked package execution
 */
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
XR_DATA const XrXirProgramSpec locked_package_program;
XR_DATA const uint32_t locked_package_entry;
XR_DATA const uint32_t locked_package_inactive;
static bool unexpected_output(void *context, const XrXirOutputGroup *group) {
    (void)context; (void)group; CHECK(false); return false;
}
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&locked_package_program,
        (XrXirProgramBudget){33554432, 64000000}, &program) == XR_XIR_OK);
    XrXirInstanceConfig config = xr_xir_instance_defaults(); XrXirInstance *instance = NULL;
    config.output = (XrXirOutputProvider){unexpected_output, NULL};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, locked_package_inactive, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(instance, locked_package_entry, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_start(instance, locked_package_inactive, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY); xr_xir_program_drop(program);
    CHECK(value.type == XR_XIR_I64 && value.payload == 41); xr_xir_value_drop(&value);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Locked package native result equals independent expectation 41; physical allocations released");
    return 0;
}
