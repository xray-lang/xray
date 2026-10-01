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
#include "xir_generic_method_old_cache.h"
XR_DATA const XrXirProgramSpec effect_source_program;
XR_DATA const uint32_t effect_source_entry;
XR_DATA const uint32_t effect_source_captured;
XR_DATA const XrXirProgramSpec method_source_program;
XR_DATA const uint32_t method_source_selected_entries[3];
XR_DATA const XrXirProgramSpec witness_value_direct_program;
XR_DATA const uint32_t witness_value_direct_selected_entry;
XR_DATA const XrXirProgramSpec witness_value_callback_program;
XR_DATA const uint32_t witness_value_callback_selected_entry;
XR_DATA const XrXirProgramSpec witness_direct_program;
XR_DATA const uint32_t witness_direct_selected_entry;
XR_DATA const XrXirProgramSpec witness_callback_program;
XR_DATA const uint32_t witness_callback_selected_entry;
XR_DATA const XrXirProgramSpec witness_inherited_program;
XR_DATA const uint32_t witness_inherited_selected_entry;
XR_DATA const XrXirProgramSpec witness_generic_method_program;
XR_DATA const uint32_t witness_generic_method_selected_entry;
XR_DATA const XrXirProgramSpec witness_generic_where_program;
XR_DATA const uint32_t witness_generic_where_selected_entry;
XR_DATA const XrXirProgramSpec witness_cross_program;
XR_DATA const uint32_t witness_cross_selected_entry;
static void native_attempt(XrXirProgram *program, uint32_t entry, int64_t expected) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll(instance).outcome.status;
    XrXirValue value = {0};
    if (status == XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == expected);
    } else CHECK(runtime_fail_at != SIZE_MAX && status == XR_XIR_CALL_OOM);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    xr_xir_value_drop(&value);
}
static void native_witness_promises(void) {
    const XrXirProgramSpec *specs[] = {&witness_direct_program,&witness_callback_program,
        &witness_inherited_program,&witness_cross_program,&witness_generic_method_program,&witness_generic_where_program,
        &witness_value_direct_program,&witness_value_callback_program};
    const uint32_t entries[] = {witness_direct_selected_entry,witness_callback_selected_entry,
        witness_inherited_selected_entry,witness_cross_selected_entry,witness_generic_method_selected_entry,witness_generic_where_selected_entry,
        witness_value_direct_selected_entry,witness_value_callback_selected_entry};
    for (uint32_t i = 0; i < sizeof(specs)/sizeof(*specs); ++i) {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_program_seal(specs[i],(XrXirProgramBudget){33554432,64000000},
            &program) == XR_XIR_OK);
        size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
        for (size_t attempt = 0; attempt <= sites; ++attempt) {
            runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
            native_attempt(program,entries[i],41);
            if (!attempt) sites = runtime_attempts;
            CHECK(runtime_live == baseline && runtime_bytes == bytes);
        }
        runtime_fail_at = SIZE_MAX; xr_xir_program_drop(program);
        CHECK(!runtime_live && !runtime_bytes);
        printf("Native witness promise %u released %zu allocation failure sites\n",i,sites);
    }
}
int main(void) {
    generic_method_old_cache(&effect_source_program);
    native_witness_promises();
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&effect_source_program,
        (XrXirProgramBudget){33554432, 64000000}, &program) == XR_XIR_OK);
    XrXirProgram *methods = NULL;
    CHECK(xr_xir_program_seal(&method_source_program,
        (XrXirProgramBudget){33554432, 64000000}, &methods) == XR_XIR_OK);
    size_t baseline = runtime_live, bytes = runtime_bytes, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        runtime_attempts = 0; runtime_fail_at = attempt ? attempt - 1 : SIZE_MAX;
        native_attempt(program, effect_source_entry, 7); native_attempt(program, effect_source_captured, 7);
        native_attempt(methods, method_source_selected_entries[0], 11);
        native_attempt(methods, method_source_selected_entries[1], 13);
        native_attempt(methods, method_source_selected_entries[2], 36);
        if (!attempt) sites = runtime_attempts;
        CHECK(runtime_live == baseline && runtime_bytes == bytes);
    }
    runtime_fail_at = SIZE_MAX; xr_xir_program_drop(program); xr_xir_program_drop(methods);
    CHECK(!runtime_live && !runtime_bytes);
    printf("Native qualified callback released %zu allocation failure sites\n", sites);
    puts("Native qualified callback returned the independent expected value 7");
    puts("Native method callbacks returned independent expected values 11, 13 and 36");
    return 0;
}
