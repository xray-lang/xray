/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_project_source_cases.h - Source-observed projection results and admission faults
 */
#ifndef XIR_CELL_PROJECT_SOURCE_CASES_H
#define XIR_CELL_PROJECT_SOURCE_CASES_H

static void cell_source_project_values(XrXirProgram *program, const CellSourceEntries *entries,
    int64_t expected) {
    XrXirInstance *instances[2] = {cell_source_instance(program), cell_source_instance(program)};
    CHECK(instances[0]->domain != instances[1]->domain);
    XrXirValue results[2] = {{0}};
    xr_xir_compile_program_drop(program);
    for (unsigned pass = 0; pass < 2; ++pass)
        CHECK(cell_source_execute(instances[pass], entries->run, &results[pass]) == XR_XIR_CALL_RETURNED);
    for (unsigned pass = 0; pass < 2; ++pass) CHECK(xr_xir_instance_free(instances[pass]) == XR_XIR_CALL_READY);
    for (unsigned pass = 0; pass < 2; ++pass) {
        cell_source_i64(&results[pass], expected);
        xr_xir_value_drop(&results[pass]);
    }
}
static void cell_source_project_fault(XrXirInstance *instance, uint32_t entry, XrXirCallStatus expected) {
    CHECK(xr_xir_instance_start(instance, entry, NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult step = xr_xir_instance_poll_bounded(instance, UINT64_MAX);
    CHECK(step.outcome.status == expected && xr_xir_instance_state(instance) == XR_XIR_INSTANCE_READY);
    CHECK(!step.outcome.value.type && !step.outcome.value.reserved && !step.outcome.value.payload);
    if (expected == XR_XIR_CALL_BOUNDS) {
        CHECK(xr_xir_fault_bounds_valid(step.outcome.panic.detail));
        CHECK(step.outcome.panic.detail.index == 0 && step.outcome.panic.detail.length == 0);
        CHECK(!step.outcome.panic.message.type && !step.outcome.panic.message.reserved &&
            !step.outcome.panic.message.payload);
    } else CHECK(expected == XR_XIR_CALL_BAD_ARGUMENT && xr_xir_panic_empty(&step.outcome.panic));
    CHECK(instance->call && !instance->call->top &&
        xr_xir_task_executor_root_idle(instance->executor));
    CHECK(instance->call->executor_owner == instance->executor &&
        !xr_xir_call_cleanup_incomplete(instance->call));
    const XrXirExecutorBinding binding = {instance->executor, instance->call->executor_activation,
        instance->call->executor_generation, instance->call->executor_ticket};
    XrXirCallStatus driver = XR_XIR_CALL_BAD_STATE;
    CHECK(xr_xir_call_driver_failure(instance->call, &binding, &driver));
    /* Valid bounds details take the language exit path. Duplicate owners take
     * the physical abort path and remain the authenticated epoch failure. */
    const XrXirCallStatus failure = expected == XR_XIR_CALL_BOUNDS ?
        XR_XIR_CALL_READY : XR_XIR_CALL_BAD_ARGUMENT;
    CHECK(driver == failure);
    CHECK(xr_xir_task_executor_completion_status(instance->executor) == failure);
    XrXirValue empty = {0};
    const XrXirCallStatus transfer = expected == XR_XIR_CALL_BOUNDS ?
        XR_XIR_CALL_BAD_STATE : XR_XIR_CALL_BAD_ARGUMENT;
    CHECK(xr_xir_instance_take_result(instance, &empty) == transfer);
    CHECK(!empty.type && !empty.reserved && !empty.payload);
    XrXirValue occupied = {XR_XIR_I64, 0, 77};
    CHECK(xr_xir_instance_take_result(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(occupied.type == XR_XIR_I64 && !occupied.reserved && occupied.payload == 77);
}
static void cell_source_project_reject(XrXirProgram *program, const CellSourceEntries *entries,
    const CellSourceCase *fixture) {
    CHECK(entries->counter != UINT32_MAX);
    XrXirInstance *instances[2] = {cell_source_instance(program), cell_source_instance(program)};
    CHECK(instances[0]->domain != instances[1]->domain);
    XrXirValue counters[4] = {{0}};
    xr_xir_compile_program_drop(program);
    for (unsigned pass = 0; pass < 2; ++pass) for (unsigned repeat = 0; repeat < 2; ++repeat) {
        cell_source_project_fault(instances[pass], entries->run, fixture->failure);
        /* A successful real getter also proves that preparation left no live loan. */
        CHECK(cell_source_execute(instances[pass], entries->counter,
            &counters[pass * 2 + repeat]) == XR_XIR_CALL_RETURNED);
    }
    for (unsigned pass = 0; pass < 2; ++pass) CHECK(xr_xir_instance_free(instances[pass]) == XR_XIR_CALL_READY);
    for (unsigned i = 0; i < 4; ++i) {
        cell_source_i64(&counters[i], fixture->expected);
        xr_xir_value_drop(&counters[i]);
    }
}
#endif // XIR_CELL_PROJECT_SOURCE_CASES_H
