/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_generic_cases.h - Independent combined specialization oracle
 */
#ifndef XIR_NOMINAL_GENERIC_CASES_H
#define XIR_NOMINAL_GENERIC_CASES_H
static void nominal_generic_cases(XrXirProgram *program) {
    XrXirInstance *instances[2] = {NULL, NULL};
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(xr_xir_instance_new(program, &config, &instances[i]) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instances[i], 0, NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instances[i], UINT64_MAX).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(instances[i], &value) == XR_XIR_CALL_RETURNED);
        CHECK(value.type == XR_XIR_I64 && value.payload == 7);
        xr_xir_value_drop(&value);
        CHECK(xr_xir_instance_free(instances[i]) == XR_XIR_CALL_READY);
    }
}
#endif // XIR_NOMINAL_GENERIC_CASES_H
