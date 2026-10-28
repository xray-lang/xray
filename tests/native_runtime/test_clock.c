/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_clock.c - Actual clock leaf failures and surviving synchronous hosts
 *
 * KEY CONCEPT:
 *   Only physical OS readings are replaced. The production runtime boundary,
 *   Source lowering and VM execution capture and retire the actual failures.
 */
#include "../lir/lir_source_fixture.h"
#include "lir/xlir_lower.h"
#include "vm/register/xrbc_lir.h"
#define xr_time_realtime_ns observed_realtime
#define xr_time_process_cpu_ns observed_cpu
#define xr_time_monotonic_ns observed_monotonic
XR_FUNC uint64_t observed_realtime(void);
XR_FUNC uint64_t observed_cpu(void);
XR_FUNC uint64_t observed_monotonic(void);
#include "runtime/native/xnative_execution.c"
#undef xr_time_realtime_ns
#undef xr_time_process_cpu_ns
#undef xr_time_monotonic_ns

static uint64_t readings[3] = {1000000, 0, 2000000};
XR_FUNC uint64_t observed_realtime(void) { return readings[0]; }
XR_FUNC uint64_t observed_cpu(void) { return readings[1]; }
XR_FUNC uint64_t observed_monotonic(void) { return readings[2]; }
static const XrtSourceLocation points[] = {
    {"clock.xr", 8, 7, 9, 7, 12}, {"outer.xr", 8, 8, 2, 8, 4}};
const XrtSourceMap xrt_standalone_source_map = {points, 2};

typedef struct ClockCall { uint32_t kind, source; bool after; int64_t result; } ClockCall;
static void invoke(void *opaque) {
    ClockCall *call = opaque;
    switch (call->kind) {
    case 1: call->result = xrt_clock_realtime_nanos(call->source); break;
    case 2: call->result = xrt_clock_process_cpu_nanos(call->source); break;
    default: call->result = xrt_clock_monotonic_nanos(call->source); break;
    }
    call->after = true;
}
static void callback(void) { fputs("EXIT_CALLBACK\n", stderr); }

static void embedded(void) {
    XrtLocationOwner *owner = NULL;
    CHECK(xrt_locations_create(points, 2, &owner) == XRT_LOCATION_OK);
    for (uint32_t kind=1; kind<=3; ++kind) {
        ClockCall call = {.kind=kind, .source=1};
        XrtExecutionFailure failure = {0};
        CHECK(xrt_run_embedded(owner, &failure, invoke, &call) == XRT_EXECUTION_COMPLETED);
        CHECK(call.after && call.result == (int64_t)readings[kind-1]);
        for (uint32_t attack=0; attack<2; ++attack) {
            if (kind == 2 && !attack) continue; // CPU zero is a real successful reading.
            uint64_t saved = readings[kind-1];
            readings[kind-1] = attack ? UINT64_MAX : 0;
            call.after = false;
            CHECK(xrt_run_embedded(owner, &failure, invoke, &call) == XRT_EXECUTION_FAILURE);
            CHECK(!call.after && failure.kind == XRT_FAILURE_HOST_CLOCK);
            CHECK(failure.detail.clock.kind == kind && failure.detail.clock.source_id == 1);
            CHECK(failure.detail.clock.reason == (attack ? XRT_CLOCK_OUT_OF_RANGE : XRT_CLOCK_READ_FAILED));
            readings[kind-1] = saved;
            CHECK(xrt_run_embedded(owner, &failure, invoke, &call) == XRT_EXECUTION_COMPLETED);
            CHECK(call.after);
        }
    }
    xrt_locations_release(owner);
}
static void nested_root(void *opaque) {
    XrtLocationOwner *outer = opaque, *inner = NULL;
    CHECK(xrt_locations_create(points, 1, &inner) == XRT_LOCATION_OK);
    XrtExecutionFailure failure = {0};
    ClockCall call = {.kind=3, .source=1};
    readings[2] = 0;
    CHECK(xrt_run_embedded(inner, &failure, invoke, &call) == XRT_EXECUTION_FAILURE);
    xrt_locations_release(inner);
    readings[2] = 2000000;
    CHECK(xrt_clock_monotonic_nanos(2) == 2000000);
    CHECK(xrt_locations_count(outer) == 2);
}
static void source_failure(void) {
    SourceFixture source = source_fixture("import time\nprint(time.nanos())\nprint(99)\n");
    XrLirLowered lir = {0};
    XrLirLowerRequest request = {source.closed, source.source.snapshot, false};
    OK(xr_lir_lower_program_checked(&request, &lir, NULL));
    XrRbcProgram *program = NULL;
    XrRbcCompileDiagnostic diagnostic = {0};
    CHECK(xr_rbc_compile(lir.module, lir.entry, &program, &diagnostic) == XR_RBC_COMPILE_OK);
    source_fixture_free(&source); xr_lir_lowered_free(&lir);
    XrRbcExecution *execution = NULL;
    CHECK(xr_rbc_program_prepare(program, NULL, 0, &execution) == XR_RBC_OK);
    readings[2] = 0;
    XrRbcOutcome *outcome = xr_rbc_execution_run(execution);
    xr_rbc_execution_free(&execution); xr_rbc_program_destroy(program);
    XrtHostClockFailure facts = {0}; const XrtLocationOwner *owner = NULL;
    CHECK(xr_rbc_outcome_status(outcome) == XR_RBC_HOST_CLOCK_FAILURE);
    CHECK(xr_rbc_outcome_host_clock_failure(outcome, &facts, &owner));
    const XrtSourceLocation *point = xrt_locations_get(owner, facts.source_id);
    CHECK(facts.kind == 3 && facts.reason == XRT_CLOCK_READ_FAILED && point &&
        point->line == 40 && point->column == 5 && strstr(point->path, "time"));
    CHECK(xrt_locations_retain((XrtLocationOwner *)owner));
    uint64_t bits = UINT64_MAX;
    CHECK(!xr_rbc_outcome_bits(outcome, &bits) && bits == UINT64_MAX);
    xr_rbc_outcome_free(&outcome);
    CHECK(xrt_locations_get(owner, facts.source_id) == point);
    xrt_locations_release((XrtLocationOwner *)owner);
    readings[2] = 2000000;
}
int main(int argc, char **argv) {
    if (argc > 1) {
        ClockCall call = {.kind=3, .source=!strcmp(argv[1], "unknown") ? 0 : 1};
        readings[2] = !strcmp(argv[1], "range") ? UINT64_MAX : 0;
        if (!strcmp(argv[1], "bad-source")) call.source = 3;
        CHECK(atexit(callback) == 0);
        xrt_run_standalone(&xrt_standalone_source_map, invoke, &call);
        fputs("AFTER_CALL\n", stderr); return 77;
    }
    embedded();
    XrtLocationOwner *owner = NULL; XrtExecutionFailure failure = {0};
    CHECK(xrt_locations_create(points, 2, &owner) == XRT_LOCATION_OK);
    CHECK(xrt_run_embedded(owner, &failure, nested_root, owner) == XRT_EXECUTION_COMPLETED);
    source_failure();
    ClockCall call = {.kind=3, .source=2};
    CHECK(xrt_run_embedded(owner, &failure, invoke, &call) == XRT_EXECUTION_COMPLETED);
    CHECK(call.after && call.result == 2000000);
    xrt_locations_release(owner);
    puts("clock scopes, Source retirement, failure owners and host retry passed");
    return 0;
}
