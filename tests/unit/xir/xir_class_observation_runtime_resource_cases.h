/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_observation_runtime_resource_cases.h - Actual scalar class allocation faults
 *
 * KEY CONCEPT:
 *   Every allocation ordinal is retried with the same published domain and stream.
 */
#ifndef XIR_CLASS_OBSERVATION_RUNTIME_RESOURCE_CASES_H
#define XIR_CLASS_OBSERVATION_RUNTIME_RESOURCE_CASES_H
typedef struct ObservationResourceOwner {
    XrXirDomain *domain;
    XrXirClassObservation *observation;
    uint64_t cursor;
    uint32_t capacity;
} ObservationResourceOwner;
static bool observation_resource_unit(const XrXirValue *value) {
    return !value->type && !value->reserved && !value->payload;
}
static void observation_resource_collect(ObservationResourceOwner *owner,
    uint64_t serial, uint32_t phase) {
    XrXirClassEvent events[128]; uint32_t count = 0; bool overflow = false;
    observation_runtime_phase(OBSERVATION_FAULT_DRAIN);
    size_t before = observation_runtime_fault.attempts;
    CHECK(xr_xir_class_observation_drain(owner->observation, &owner->cursor,
        events, 128, &count, &overflow) == XR_XIR_VALUE_OK);
    CHECK(observation_runtime_fault.attempts == before);
    if (!phase) { CHECK(!count && !overflow && !owner->cursor); return; }
    if (phase == XR_XIR_CLASS_CONSTRUCT) {
        CHECK(count == 1 && !overflow);
        observation_core_event(events, serial, XR_XIR_CLASS_CONSTRUCT, 0, 0, XR_XIR_VALUE_OK);
    } else if (owner->capacity == 1) {
        CHECK(!count && overflow && owner->cursor == 1);
    } else {
        CHECK(count == 2 && !overflow && owner->cursor == 3);
        observation_core_event(events, serial, XR_XIR_CLASS_FINALIZE, 0, 0, XR_XIR_VALUE_OK);
        observation_core_event(events + 1, serial, XR_XIR_CLASS_RECLAIM, 0, 0, XR_XIR_VALUE_OK);
    }
}
static XrXirValueStatus observation_resource_operation(ObservationResourceOwner *owner,
    XrXirTypeArena *arena, uint64_t serial) {
    XrXirValueStatus status = XR_XIR_VALUE_OK;
    XrXirValue text = {0}, object = {0}, alias = {0};
    if (!owner->domain) {
        XrXirDomainBudgetControls limits = {65536, 65536, 65536, 65536, 65536};
        observation_runtime_phase(OBSERVATION_FAULT_DOMAIN);
        status = xr_xir_domain_new_budgeted(65536, &limits, &owner->domain);
        if (status != XR_XIR_VALUE_OK) { CHECK(!owner->domain); return status; }
    }
    if (!owner->observation) {
        observation_runtime_phase(OBSERVATION_FAULT_OPEN);
        status = xr_xir_domain_class_observation_open(owner->domain, owner->capacity, &owner->observation);
        if (status != XR_XIR_VALUE_OK) { CHECK(!owner->observation); return status; }
    }
    observation_runtime_phase(OBSERVATION_FAULT_TEXT);
    status = xr_xir_string_new(owner->domain, "kept", 4, &text);
    if (status != XR_XIR_VALUE_OK) { CHECK(observation_resource_unit(&text)); goto failed; }
    XrXirValue fields[] = {{XR_XIR_I64, 0, 40}, text};
    XrXirValueAdmission admission = {arena, owner->domain, NULL, NULL, 65536, 65536};
    observation_runtime_phase(OBSERVATION_FAULT_CLASS);
    status = xr_xir_class_new((XrXirType)PREFIX_CLASS_T, fields, 2, &admission, &object);
    if (status != XR_XIR_VALUE_OK) { CHECK(observation_resource_unit(&object)); goto failed; }
    observation_runtime_phase(OBSERVATION_FAULT_ALIAS);
    CHECK(xr_xir_value_copy(&object, &alias) == XR_XIR_VALUE_OK);
    observation_resource_collect(owner, serial, XR_XIR_CLASS_CONSTRUCT);
    observation_runtime_phase(OBSERVATION_FAULT_DROP);
    xr_xir_value_drop(&object); CHECK(xr_xir_value_valid(&alias)); xr_xir_value_drop(&alias);
    observation_resource_collect(owner, serial, XR_XIR_CLASS_RECLAIM);
    observation_runtime_phase(OBSERVATION_FAULT_DROP); xr_xir_value_drop(&text);
    CHECK(observation_resource_unit(&text) && observation_resource_unit(&object) && observation_resource_unit(&alias));
    return XR_XIR_VALUE_OK;
failed:
    observation_runtime_phase(OBSERVATION_FAULT_DROP);
    xr_xir_value_drop(&alias); xr_xir_value_drop(&object); xr_xir_value_drop(&text);
    CHECK(observation_resource_unit(&text) && observation_resource_unit(&object) && observation_resource_unit(&alias));
    observation_resource_collect(owner, 0, 0); return status;
}
static void observation_resource_release(ObservationResourceOwner *owner) {
    observation_runtime_fault_end();
    if (owner->domain) { xr_xir_domain_close(owner->domain); xr_xir_domain_drop(owner->domain); }
    xr_xir_class_observation_drop(&owner->observation);
    *owner = (ObservationResourceOwner){0};
}
static void observation_resource_failed_owner(const ObservationResourceOwner *owner) {
    if (!owner->domain) { CHECK(!owner->observation); return; }
    XrXirDomainStats values = xr_xir_domain_stats(owner->domain);
    XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(owner->domain);
    CHECK(values.live_bytes == sizeof(XrXirDomain) && !budget.call_live);
    uint64_t metadata = owner->observation ? sizeof(XrXirClassObservation) +
        (uint64_t)owner->capacity * sizeof(XrXirClassEvent) : 0;
    CHECK(budget.metadata_live == metadata && !owner->cursor);
}
static void observation_resource_census(const ObservationRuntimeFault *fault) {
    CHECK(!fault->hits && fault->phase_attempts[OBSERVATION_FAULT_DOMAIN] == 1);
    CHECK(fault->phase_attempts[OBSERVATION_FAULT_OPEN] == 1 && fault->phase_attempts[OBSERVATION_FAULT_TEXT] == 2);
    CHECK(fault->phase_attempts[OBSERVATION_FAULT_CLASS] == 1);
    CHECK(!fault->phase_attempts[OBSERVATION_FAULT_ALIAS] && !fault->phase_attempts[OBSERVATION_FAULT_DROP] &&
        !fault->phase_attempts[OBSERVATION_FAULT_DRAIN]);
    CHECK(fault->attempts && fault->attempts < 65536);
}
static void observation_resource_mode(ValueCompileOwner *compiler, XrXirTypeArena *arena, uint32_t mode) {
    size_t blocks = cell_owner_live, bytes = cell_owner_bytes;
    size_t compiler_blocks = value_compile_live, compiler_bytes = value_compile_bytes;
    XrCompileResourceStats compiler_before = value_compile_stats(&compiler->context);
    ObservationResourceOwner census_owner = {.capacity = mode ? 1u : 128u};
    observation_runtime_fault_begin(SIZE_MAX);
    CHECK(observation_resource_operation(&census_owner, arena, 1) == XR_XIR_VALUE_OK);
    ObservationRuntimeFault census = observation_runtime_fault; observation_resource_census(&census);
    XrXirDomainStats values = xr_xir_domain_stats(census_owner.domain);
    XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(census_owner.domain);
    printf("CLASS_CORE_CENSUS mode%u sites%llu requested%llu call_requested%llu work%llu value_peak%llu metadata_peak%llu call_peak%llu\n",
        mode, (unsigned long long)census.attempts, (unsigned long long)budget.requested_bytes,
        (unsigned long long)budget.requested_call_bytes, (unsigned long long)budget.work,
        (unsigned long long)values.peak_bytes, (unsigned long long)budget.metadata_peak,
        (unsigned long long)budget.call_peak);
    observation_resource_release(&census_owner);
    CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
    for (size_t ordinal = 0; ordinal < census.attempts; ++ordinal) {
        ObservationResourceOwner owner = {.capacity = mode ? 1u : 128u};
        observation_runtime_fault_begin(ordinal);
        CHECK(observation_resource_operation(&owner, arena, 1) == XR_XIR_VALUE_OOM);
        CHECK(observation_runtime_fault.hits == 1 && observation_runtime_fault.attempts == ordinal + 1);
        uint32_t failed_phase = observation_runtime_fault.failed_phase;
        observation_resource_failed_owner(&owner);
        XrXirDomain *domain = owner.domain; XrXirClassObservation *observation = owner.observation;
        /* Failed class storage has already spent a serial and its 289-work reservation. */
        uint64_t serial = failed_phase == OBSERVATION_FAULT_CLASS ? 2u : 1u;
        observation_runtime_fault_end(); observation_runtime_fault_begin(SIZE_MAX);
        CHECK(observation_resource_operation(&owner, arena, serial) == XR_XIR_VALUE_OK);
        CHECK(!observation_runtime_fault.hits && (!domain || owner.domain == domain) &&
            (!observation || owner.observation == observation));
        observation_resource_release(&owner);
        CHECK(cell_owner_live == blocks && cell_owner_bytes == bytes);
        CHECK(value_compile_live == compiler_blocks && value_compile_bytes == compiler_bytes);
    }
    XrCompileResourceStats compiler_after = value_compile_stats(&compiler->context);
    CHECK(compiler_after.live_bytes == compiler_before.live_bytes && compiler_after.work == compiler_before.work);
    CHECK(value_compile_live == compiler_blocks && value_compile_bytes == compiler_bytes);
}
static void observation_resource_cases(ValueCompileOwner *compiler, XrXirTypeArena *arena) {
    observation_resource_mode(compiler, arena, 0); observation_resource_mode(compiler, arena, 1);
}
#endif // XIR_CLASS_OBSERVATION_RUNTIME_RESOURCE_CASES_H
