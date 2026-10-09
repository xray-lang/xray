/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_loan_lifecycle_cases.h - Public Instance actions and Source observations
 */
#ifndef XIR_CELL_LOAN_LIFECYCLE_CASES_H
#define XIR_CELL_LOAN_LIFECYCLE_CASES_H
enum { CELL_LIFE_RETURN, CELL_LIFE_THROW, CELL_LIFE_PANIC, CELL_LIFE_UNCAUGHT,
    CELL_LIFE_RESUME, CELL_LIFE_CANCEL, CELL_LIFE_STOP, CELL_LIFE_CLOSE, CELL_LIFE_REENTRANT,
    CELL_LIFE_MODES };
enum { CELL_ENTRY_RUN, CELL_ENTRY_COUNTER, CELL_ENTRY_THROW, CELL_ENTRY_PANIC,
    CELL_ENTRY_UNCAUGHT, CELL_ENTRY_SUSPENDED, CELL_LIFE_ENTRIES };
typedef struct CellLifecycleLog {
    const int64_t *expected;
    uint32_t count, at, reentered;
    const uint32_t *entries;
    XrXirInstance *active, *other;
    XrXirValue other_result, other_counter;
    bool reentrant;
} CellLifecycleLog;

static void cell_life_number(const XrXirValue *value, int64_t expected) {
    CHECK(xr_xir_value_valid(value) && value->type == XR_XIR_I64 && !value->reserved);
    CHECK(value->payload == expected);
}
static void cell_life_problem(const XrXirValue *value) {
    /* The thrown concrete enum owns its metadata beyond Program and Instance. */
    CHECK(xr_xir_value_valid(value) && !value->reserved);
    uint32_t variant = UINT32_MAX;
    XrXirEnumBorrow borrowed = {0};
    CHECK(xr_xir_enum_variant(value, &variant) == XR_XIR_VALUE_OK && variant == 0);
    CHECK(xr_xir_enum_borrow(value, &borrowed) == XR_XIR_VALUE_OK);
    CHECK(borrowed.name.length == 7 && borrowed.name.bytes && !memcmp(borrowed.name.bytes, "Problem", 7));
    CHECK(borrowed.member.length == 6 && borrowed.member.bytes && !memcmp(borrowed.member.bytes, "Failed", 6));
    CHECK(borrowed.field_count == 0);
    printf("Cell lifecycle owned thrown type=%u variant=%u Problem.Failed fields=0 after Instance free\n",
        value->type, variant);
}
static XrXirCallStatus cell_life_run(XrXirInstance *instance, uint32_t entry, XrXirValue *output) {
    CHECK(output && !output->type && !output->reserved && !output->payload);
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    if (status == XR_XIR_CALL_RETURNED || status == XR_XIR_CALL_THROWN)
        CHECK(xr_xir_instance_take_result(instance, output) == status && xr_xir_value_valid(output));
    return status;
}
static XrXirOutputStatus cell_life_output(void *context, const XrXirOutputGroup *group) {
    CellLifecycleLog *log = context;
    CHECK(group && group->line && group->stream == XR_XIR_STDOUT && group->count == 1);
    CHECK(log->at < log->count);
    cell_life_number(&group->values[0], log->expected[log->at++]);
    if (log->reentrant && !log->reentered) {
        ++log->reentered;
        CHECK(log->active && log->other && log->entries);
        CHECK(xr_xir_instance_start(log->active, log->entries[CELL_ENTRY_COUNTER], NULL, 0) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_instance_poll_bounded(log->active, UINT64_MAX).outcome.status == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_instance_cancel_current(log->active) == XR_XIR_CALL_BUSY);
        CHECK(xr_xir_instance_free(log->active) == XR_XIR_CALL_BUSY);
        CHECK(cell_life_run(log->other, log->entries[CELL_ENTRY_RUN], &log->other_result) == XR_XIR_CALL_RETURNED);
        CHECK(cell_life_run(log->other, log->entries[CELL_ENTRY_COUNTER], &log->other_counter) == XR_XIR_CALL_RETURNED);
        cell_life_number(&log->other_result, 2);
        cell_life_number(&log->other_counter, 2);
    }
    return XR_XIR_OUTPUT_OK;
}
static XrXirInstance *cell_life_instance(XrXirProgram *program, CellLifecycleLog *log) {
    XrXirInstanceConfig config;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    if (log) config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, cell_life_output, log};
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY && instance);
    return instance;
}
static void cell_life_cost(XrXirInstance *instance, unsigned mode) {
    /* Observation of the real budget supplies no execution or borrowing authority. */
    XrXirDomainBudgetStats stats = xr_xir_domain_budget_stats(instance->domain);
    printf("Cell lifecycle mode%u actual work=%llu value_request=%llu call_request=%llu call_peak=%llu metadata_peak=%llu\n",
        mode, (unsigned long long)stats.work, (unsigned long long)stats.requested_bytes,
        (unsigned long long)stats.requested_call_bytes, (unsigned long long)stats.call_peak,
        (unsigned long long)stats.metadata_peak);
    CHECK(stats.bound && stats.work <= stats.work_limit && stats.requested_bytes <= stats.requested_limit);
}
static void cell_life_terminal(XrXirInstance *instance, const uint32_t *entries,
    unsigned mode, XrXirValue *result) {
    uint32_t entry = mode == CELL_LIFE_RETURN ? CELL_ENTRY_RUN : mode == CELL_LIFE_THROW ? CELL_ENTRY_THROW :
        mode == CELL_LIFE_PANIC ? CELL_ENTRY_PANIC : CELL_ENTRY_UNCAUGHT;
    XrXirCallStatus status = cell_life_run(instance, entries[entry], result);
    CHECK(status == (mode == CELL_LIFE_UNCAUGHT ? XR_XIR_CALL_THROWN : XR_XIR_CALL_RETURNED));
    XrXirValue counter = {0};
    CHECK(cell_life_run(instance, entries[CELL_ENTRY_COUNTER], &counter) == XR_XIR_CALL_RETURNED);
    cell_life_number(&counter, mode == CELL_LIFE_RETURN ? 2 : mode == CELL_LIFE_UNCAUGHT ? 11 : 12);
    xr_xir_value_drop(&counter);
}
static void cell_life_suspended(XrXirInstance **instance, const uint32_t *entries,
    unsigned mode, XrXirValue *result) {
    CHECK(xr_xir_instance_start(*instance, entries[CELL_ENTRY_SUSPENDED], NULL, 0) == XR_XIR_CALL_READY);
    XrXirInstanceResult step = xr_xir_instance_poll_bounded(*instance, UINT64_MAX);
    CHECK(step.outcome.status == XR_XIR_CALL_SUSPENDED);
    CHECK(xr_xir_instance_start(*instance, entries[CELL_ENTRY_COUNTER], NULL, 0) == XR_XIR_CALL_BUSY);
    if (mode == CELL_LIFE_CLOSE) {
        CHECK(xr_xir_instance_free(*instance) == XR_XIR_CALL_READY);
        *instance = NULL;
        return;
    }
    if (mode == CELL_LIFE_STOP) {
        CHECK(xr_xir_instance_stop(*instance) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(*instance, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED);
        CHECK(xr_xir_instance_start(*instance, entries[CELL_ENTRY_COUNTER], NULL, 0) == XR_XIR_CALL_BAD_STATE);
        return;
    }
    bool cancelled = mode == CELL_LIFE_CANCEL;
    if (cancelled) CHECK(xr_xir_instance_cancel_current(*instance) == XR_XIR_CALL_CANCEL_REQUESTED);
    else CHECK(xr_xir_instance_resume(*instance, step.epoch, step.outcome.wake) == XR_XIR_CALL_READY);
    XrXirCallStatus status = xr_xir_instance_poll_bounded(*instance, UINT64_MAX).outcome.status;
    CHECK(status == (cancelled ? XR_XIR_CALL_CANCELLED : XR_XIR_CALL_RETURNED));
    if (!cancelled) CHECK(xr_xir_instance_take_result(*instance, result) == XR_XIR_CALL_RETURNED);
    XrXirValue counter = {0}, next = {0};
    CHECK(cell_life_run(*instance, entries[CELL_ENTRY_COUNTER], &counter) == XR_XIR_CALL_RETURNED);
    cell_life_number(&counter, cancelled ? 111 : 117);
    CHECK(cell_life_run(*instance, entries[CELL_ENTRY_RUN], &next) == XR_XIR_CALL_RETURNED);
    cell_life_number(&next, cancelled ? 113 : 119);
    xr_xir_value_drop(&counter);
    xr_xir_value_drop(&next);
}
#endif // XIR_CELL_LOAN_LIFECYCLE_CASES_H
