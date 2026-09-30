/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_module_forest_cases.h - Independent initialization and entry expectations
 */
#ifndef XIR_MODULE_FOREST_CASES_H
#define XIR_MODULE_FOREST_CASES_H
typedef struct ForestLog { uint32_t outputs, begins[3], releases; } ForestLog;
static bool forest_write(void *context, const XrXirOutputGroup *group) {
    ForestLog *log = context;
    const int64_t expected[] = {11, 22};
    CHECK(log->outputs < 2 && group->count == 1);
    CHECK(group->values[0].type == XR_XIR_I64 && group->values[0].payload == expected[log->outputs]);
    ++log->outputs;
    return true;
}
static void forest_trace(void *context, XrXirLifecycleEvent event, uint32_t index) {
    ForestLog *log = context;
    if (event == XR_XIR_MODULE_BEGIN) { CHECK(index < 3); ++log->begins[index]; }
    if (event == XR_XIR_SLOT_RELEASED) ++log->releases;
}
static void module_forest_cases(XrXirProgram *program) {
    ForestLog log = {0};
    XrXirInstanceConfig config = xr_xir_instance_defaults();
    config.output = (XrXirOutputProvider){forest_write, &log};
    config.trace = forest_trace; config.trace_context = &log;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(!log.outputs && !log.begins[0] && !log.begins[1] && !log.begins[2]);
    const uint32_t entries[] = {3, 5};
    for (uint32_t i = 0; i < 2; ++i) {
        CHECK(xr_xir_instance_start(instance, entries[i], NULL, 0) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
        XrXirValue result = {0};
        CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
        CHECK(result.type == XR_XIR_I64 && result.payload == 41); xr_xir_value_drop(&result);
        CHECK(xr_xir_instance_start(instance, 4, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    }
    CHECK(log.outputs == 2 && !log.begins[0] && log.begins[1] == 1 && log.begins[2] == 1);
    CHECK(xr_xir_instance_start(instance, 6, NULL, 0) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue function = {0};
    CHECK(xr_xir_instance_take_result(instance, &function) == XR_XIR_CALL_RETURNED);
    XrXirFunctionBinding *binding = (XrXirFunctionBinding *)xr_xir_function_binding(&function);
    CHECK(binding && binding->entry == 5);
    binding->entry = 4;
    CHECK(xr_xir_instance_start_function(instance, &function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(xr_xir_instance_start(instance, 7, &function, 1) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(log.outputs == 2 && !log.begins[0]);
    binding->entry = 5;
    CHECK(xr_xir_instance_start(instance, 7, &function, 1) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_poll(instance).outcome.status == XR_XIR_CALL_RETURNED);
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == XR_XIR_CALL_RETURNED);
    CHECK(result.type == XR_XIR_I64 && result.payload == 41);
    xr_xir_value_drop(&result); xr_xir_value_drop(&function);
    CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
    CHECK(!log.releases); xr_xir_program_drop(program);
    CHECK(!runtime_live && !runtime_bytes);
}
#endif // XIR_MODULE_FOREST_CASES_H
