/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_class_observation_runtime_faults.h - Observe actual value allocation attempts
 *
 * KEY CONCEPT:
 *   The value implementation is included once; fault injection never owns its storage.
 */
#ifndef XIR_CLASS_OBSERVATION_RUNTIME_FAULTS_H
#define XIR_CLASS_OBSERVATION_RUNTIME_FAULTS_H
enum { OBSERVATION_FAULT_DOMAIN = 1, OBSERVATION_FAULT_OPEN, OBSERVATION_FAULT_TEXT,
    OBSERVATION_FAULT_CLASS, OBSERVATION_FAULT_ALIAS, OBSERVATION_FAULT_DROP,
    OBSERVATION_FAULT_DRAIN, OBSERVATION_FAULT_PHASES };
typedef struct ObservationRuntimeFault {
    size_t attempts, fail_at, hits, phase_attempts[OBSERVATION_FAULT_PHASES];
    uint32_t phase, failed_phase;
    bool active;
} ObservationRuntimeFault;
static ObservationRuntimeFault observation_runtime_fault;
static bool observation_runtime_allocation_fail(void) {
    ObservationRuntimeFault *fault = &observation_runtime_fault;
    if (!fault->active) return false;
    CHECK(fault->phase && fault->phase < OBSERVATION_FAULT_PHASES && fault->attempts != SIZE_MAX);
    CHECK(fault->phase_attempts[fault->phase] != SIZE_MAX);
    ++fault->phase_attempts[fault->phase];
    if (fault->attempts++ != fault->fail_at) return false;
    ++fault->hits; fault->failed_phase = fault->phase; return true;
}
static void observation_runtime_fault_begin(size_t fail_at) {
    CHECK(!observation_runtime_fault.active);
    observation_runtime_fault = (ObservationRuntimeFault){.fail_at = fail_at, .active = true};
}
static void observation_runtime_phase(uint32_t phase) {
    CHECK(phase && phase < OBSERVATION_FAULT_PHASES);
    observation_runtime_fault.phase = phase;
}
static void observation_runtime_fault_end(void) { observation_runtime_fault.active = false; }
#define CELL_OWNER_ALLOCATION_FAIL() observation_runtime_allocation_fail()
#endif // XIR_CLASS_OBSERVATION_RUNTIME_FAULTS_H
