/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_path_program_cases.h - Independent projected value and ownership oracles
 */
#ifndef XIR_PATH_PROGRAM_CASES_H
#define XIR_PATH_PROGRAM_CASES_H
#include "xir/xxir_array.h"
#include "xir/xxir_struct.h"
static XrXirOutputStatus path_program_output(void *context, const XrXirOutputGroup *group) {
    unsigned *calls = context; ++*calls;
    const int64_t expected[] = {99,99,84,61,2,11};
    CHECK(group->line && group->stream == XR_XIR_STDOUT && group->count == 7);
    for (unsigned i = 0; i < 6; ++i)
        CHECK(group->values[i].type == XR_XIR_I64 && group->values[i].payload == expected[i]);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&group->values[6],&bytes,&length) && length == 6 && !memcmp(bytes,"edited",6));
    return XR_XIR_OUTPUT_OK;
}
static XrXirValue path_program_result(XrXirInstance *instance, uint32_t entry) {
    CHECK(xr_xir_instance_start(instance,entry,NULL,0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance,&result) == XR_XIR_CALL_RETURNED);
    return result;
}
static void path_program_value(XrXirValue *root, bool changed) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {xr_xir_value_arena(root),domain,NULL,NULL,10000,65536};
    XrXirValue pair = {0}, items = {0}, label = {0}, value = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(root,0,&admission,&pair,&fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_get(&pair,0,&admission,&items) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_struct_get(&pair,1,&admission,&label) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(root); xr_xir_value_drop(&pair);
    int64_t length = 0;
    CHECK(xr_xir_array_len(&items,&admission,&length) == XR_XIR_VALUE_OK && length == (changed ? 2 : 1));
    CHECK(xr_xir_array_get(&items,0,&admission,&value,&fault) == XR_XIR_VALUE_OK);
    CHECK(value.type == XR_XIR_I64 && value.payload == (changed ? 84 : 73)); xr_xir_value_drop(&value);
    if (changed) {
        CHECK(xr_xir_array_get(&items,1,&admission,&value,&fault) == XR_XIR_VALUE_OK);
        CHECK(value.type == XR_XIR_I64 && value.payload == 61); xr_xir_value_drop(&value);
    }
    const char *bytes = NULL; size_t count = 0;
    CHECK(xr_xir_string_view(&label,&bytes,&count) && count == (changed ? 6u : 3u));
    CHECK(!memcmp(bytes,changed ? "edited" : "new",count));
    xr_xir_value_drop(&items); xr_xir_value_drop(&label);
    xr_xir_domain_drop(domain);
}
static void path_program_cases(XrXirProgram *program, unsigned kind) {
    XrXirInstance *instances[2] = {0}; XrXirValue escaped[2] = {0}, slots[2] = {0}; unsigned outputs = 0;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, path_program_output, &outputs};
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(program,&config,&instances[i]) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        escaped[i] = path_program_result(instances[i],1);
        for (uint32_t f = 5; f < 7; ++f) for (unsigned bad = 0; bad < 2; ++bad) {
            XrXirValue argument = {XR_XIR_I64,0,bad ? INT64_MAX : -1};
            CHECK(xr_xir_instance_start(instances[i],f,&argument,1) == XR_XIR_CALL_READY);
            XrXirCallResult failure = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome;
            CHECK(failure.status == XR_XIR_CALL_BOUNDS && failure.panic.detail.code == 430);
            CHECK(failure.panic.detail.index == argument.payload && failure.panic.detail.length == (kind ? 1 : 2));
            CHECK(failure.value.type == XR_XIR_UNIT && !failure.value.payload && !failure.wake);
            CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_READY);
        }
        slots[i] = path_program_result(instances[i],3);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(outputs == 2);
    for (unsigned i = 0; i < 2; ++i) {
        path_program_value(&escaped[i],true);
        path_program_value(&slots[i],kind == 0);
    }
}
#endif // XIR_PATH_PROGRAM_CASES_H
