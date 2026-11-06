/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_fault_lifecycle.h - Assert ordinary array execution failure ownership
 */
#ifndef XIR_ARRAY_FAULT_LIFECYCLE_H
#define XIR_ARRAY_FAULT_LIFECYCLE_H
static inline bool array_failure_take(XrXirInstance *instance, uint64_t previous_epoch,
    XrXirCallStatus failure, XrXirValue *output, const char *site, size_t ordinal) {
    CHECK(failure == XR_XIR_CALL_OOM || failure == XR_XIR_CALL_LIMIT);
    const XrXirInstance *fault_instance = instance_current(instance);
    CHECK(fault_instance);
    XrXirCallStatus expected = XR_XIR_CALL_BAD_STATE;
    bool attached_failure = false;
    if (fault_instance->epoch != previous_epoch) {
        CHECK(fault_instance->epoch == previous_epoch + 1);
        if (fault_instance->call && fault_instance->state != XR_XIR_INSTANCE_FAILED) {
            CHECK(xr_xir_task_executor_root_idle(fault_instance->executor));
            CHECK(xr_xir_task_executor_completion_status(fault_instance->executor) == failure);
            expected = failure; attached_failure = true;
        }
    }
    const XrXirCallStatus taken = xr_xir_instance_take_result(instance, output);
    if (taken != expected || output->type || output->payload)
        fprintf(stderr, "array failure %s ordinal%zu expected%u take%u terminal%u epochBefore%llu epochAfter%llu state%u call%u idle%u completion%u budgetExhausted%u type%u payload%lld\n",
            site, ordinal, expected, taken, failure, (unsigned long long)previous_epoch,
            (unsigned long long)fault_instance->epoch, fault_instance->state, fault_instance->call != NULL,
            fault_instance->executor && xr_xir_task_executor_root_idle(fault_instance->executor), fault_instance->executor ?
                (unsigned)xr_xir_task_executor_completion_status(fault_instance->executor) : UINT32_MAX,
            instance_root(instance)->budget.exhausted, output->type, (long long)output->payload);
    CHECK(taken == expected && !output->type && !output->payload);
    return attached_failure;
}
/* Only attempts without an intervening successful call use this retirement
 * assertion. A recovered activation still requires ordinary READY retirement. */
static inline void array_failure_free(XrXirInstance *instance, bool attached_failure,
    XrXirCallStatus failure, size_t blocks, size_t bytes, const char *site) {
    CHECK(failure == XR_XIR_CALL_OOM || failure == XR_XIR_CALL_LIMIT);
    const XrXirInstance *fault_instance = instance_current(instance);
    CHECK(fault_instance);
    const XrXirCallStatus expected = attached_failure ? failure : XR_XIR_CALL_READY;
    const unsigned state = (unsigned)fault_instance->state;
    const unsigned completion = fault_instance->executor ?
        (unsigned)xr_xir_task_executor_completion_status(fault_instance->executor) : UINT32_MAX;
    const bool exhausted = instance_root(instance)->budget.exhausted;
    if (attached_failure) CHECK(completion == (unsigned)failure);
    const XrXirCallStatus freed = xr_xir_instance_free(instance);
    if (freed != expected || runtime_live != blocks || runtime_bytes != bytes)
        fprintf(stderr, "array retirement %s expected%u free%u preFreeState%u preFreeCompletion%u budgetExhausted%u blocks%zu/%zu bytes%zu/%zu\n",
            site, expected, freed, state, completion, exhausted, runtime_live, blocks, runtime_bytes, bytes);
    CHECK(freed == expected && runtime_live == blocks && runtime_bytes == bytes);
}
#endif // XIR_ARRAY_FAULT_LIFECYCLE_H
