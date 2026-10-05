/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_enum_ops_cases.h - Independent enum output and escaped-result expectations
 */
#ifndef XIR_ENUM_OPS_CASES_H
#define XIR_ENUM_OPS_CASES_H
#include "xir/xxir_enum.h"
static XrXirOutputStatus enum_ops_output(void *context, const XrXirOutputGroup *group) {
    unsigned *calls = context; ++*calls;
    CHECK(group->stream == XR_XIR_STDOUT && group->line && group->count == 4);
    const int64_t expected[] = {1,23,0};
    for (uint32_t i = 0; i < 3; ++i) CHECK(group->values[i].type == XR_XIR_I64 && group->values[i].payload == expected[i]);
    const char *bytes = NULL; size_t count = 0;
    CHECK(xr_xir_string_view(&group->values[3],&bytes,&count) && count == 11 && !memcmp(bytes,"constructed",11));
    return XR_XIR_OUTPUT_OK;
}
static void enum_ops_cases(XrXirProgram *program) {
    XrXirInstance *instances[2] = {0}; unsigned outputs = 0; XrXirValue escaped = {0};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, enum_ops_output, &outputs};
    for (unsigned i = 0; i < 2; ++i) CHECK(xr_xir_instance_new(program,&config,&instances[i]) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i],2,NULL,0) == XR_XIR_CALL_READY);
        XrXirInstanceResult wait = xr_xir_instance_poll_bounded(instances[i], UINT64_MAX); CHECK(wait.outcome.status == XR_XIR_CALL_SUSPENDED);
        if (!i) {
            CHECK(xr_xir_instance_resume(instances[i],wait.epoch,wait.outcome.wake) == XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
            CHECK(xr_xir_instance_take_result(instances[i],&escaped) == XR_XIR_CALL_RETURNED);
        }
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
    CHECK(outputs == 2); uint32_t variant = 99;
    CHECK(xr_xir_enum_variant(&escaped,&variant) == XR_XIR_VALUE_OK && variant == 1);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {xr_xir_value_arena(&escaped),domain,NULL,NULL,10000,65536};
    XrXirValue field = {0}; CHECK(xr_xir_enum_get(&escaped,1,0,&admission,&field) == XR_XIR_VALUE_OK);
    CHECK(field.type == XR_XIR_I64 && field.payload == 23); xr_xir_value_drop(&field);
    CHECK(xr_xir_enum_get(&escaped,1,1,&admission,&field) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&escaped); xr_xir_domain_drop(domain);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&field,&bytes,&length) && length == 11 && !memcmp(bytes,"constructed",11));
    xr_xir_value_drop(&field);
}
static void enum_wrong_variant_cases(XrXirProgram *program) {
    unsigned outputs = 0; XrXirInstance *instance = NULL;
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, enum_ops_output, &outputs};
    CHECK(xr_xir_instance_new(program,&config,&instance) == XR_XIR_CALL_READY); xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,2,NULL,0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status == XR_XIR_CALL_BAD_STATE && outputs == 0);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
}
#endif // XIR_ENUM_OPS_CASES_H
