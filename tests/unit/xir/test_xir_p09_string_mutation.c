/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_mutation.c - String mutation across real execution boundaries
 *
 * KEY CONCEPT:
 *   Independent expectations survive COW, allocation failures and cancellation.
 */
#include "base/xmalloc.h"
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_program_internal.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_emit_c.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#if defined(XR_CAPACITY_NATIVE)
XR_DATA const XrXirProgramSpec array_capacity_program;
#endif
#include "xir_p09_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_array_capacity_pipeline.h"

static void string_mutation_compiler(unsigned mode) {
    CapacityCompile baseline = capacity_build(SIZE_MAX, capacity_limits(), mode, NULL);
    CHECK(baseline.status == XR_XIR_OK && baseline.program && baseline.sites);
    const size_t sites = baseline.sites; const XrCompileResourceStats stats = baseline.stats;
    capacity_release(&baseline);
    for (size_t failure = 0; failure < sites; ++failure) {
        CapacityCompile rejected = capacity_build(failure, capacity_limits(), mode, NULL);
        if (!instance_compile_injected || rejected.status != XR_XIR_OUT_OF_MEMORY || rejected.program)
            fprintf(stderr, "string-mutation compiler failure%zu status%u stage%u attempts%zu injected%u\n",
                failure, rejected.status, rejected.stage, rejected.sites, instance_compile_injected);
        CHECK(instance_compile_injected && rejected.sites > failure &&
            rejected.status == XR_XIR_OUT_OF_MEMORY && !rejected.program && !rejected.lowered);
        capacity_release(&rejected);
        if (!(failure % 512)) { printf("string-mutation mode%u compiler FI %zu/%zu physical=0/0\n", mode, failure + 1, sites); fflush(stdout); }
    }
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        XrCompileResourceLimits limits = capacity_limits();
        uint64_t exact = axis == 0 ? stats.allocated_bytes : axis == 1 ? stats.peak_bytes : stats.work;
        CHECK(exact > 1);
        if (axis == 0) limits.allocated_bytes = exact - shortfall;
        else if (axis == 1) limits.live_bytes = exact - shortfall;
        else limits.work = exact - shortfall;
        CapacityCompile run = capacity_build(SIZE_MAX, limits, mode, NULL);
        CHECK(run.status == (shortfall ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(shortfall ? !run.program && !run.lowered : run.program != NULL);
        CHECK(run.stats.allocated_bytes <= limits.allocated_bytes && run.stats.peak_bytes <= limits.live_bytes && run.stats.work <= limits.work);
        capacity_release(&run);
    }
    printf("string-mutation mode%u compiler FI complete actual-sites%zu allocated%llu peak%llu work%llu six boundaries physical=0/0\n",
        mode, sites, (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes,
        (unsigned long long)stats.work);
}
static uint32_t string_mutation_find(const CapacityCompile *run, const char *name) {
    for (uint32_t f = 0; f < run->module->function_count; ++f) {
        const XrXirFunction *function = &run->module->functions[f];
        if (run->module->declarations->functions[f].module == run->module->declarations->root_module &&
            function->name_length == strlen(name) && !memcmp(function->name, name, strlen(name))) return f;
    }
    CHECK(false); return UINT32_MAX;
}
static XrXirCallStatus string_mutation_poll(XrXirInstance *instance, uint32_t function) {
    XrXirCallStatus status = xr_xir_instance_start(instance, function, NULL, 0);
    while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    return status;
}
static XrXirValue string_mutation_result(XrXirInstance *instance, uint32_t function) {
    CHECK(string_mutation_poll(instance, function) == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static XrXirInstance *string_mutation_open(CapacityCompile *run) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576; XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue value = string_mutation_result(instance, run->program->declarations->entry_function);
    CHECK(value.type == XR_XIR_I64 && !value.payload); xr_xir_value_drop(&value); return instance;
}
static void string_mutation_answer(CapacityCompile *run, XrXirInstance *instance) {
    XrXirValue value = string_mutation_result(instance, string_mutation_find(run, "scenario"));
    CHECK(value.type == XR_XIR_I64 && value.payload == 66); xr_xir_value_drop(&value);
}
static void string_mutation_failures(CapacityCompile *run, unsigned mode) {
    XrXirInstance *first = string_mutation_open(run), *second = string_mutation_open(run);
    runtime_attempts = 0; string_mutation_answer(run, first); const size_t sites = runtime_attempts; CHECK(sites);
    string_mutation_answer(run, second);
    CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);
    for (size_t failure = 0; failure < sites; ++failure) {
        first = string_mutation_open(run); second = string_mutation_open(run);
        XrXirDomain *domain = first->domain; const XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
        runtime_attempts = 0; runtime_fail_at = failure;
        XrXirCallStatus status = xr_xir_instance_start(first, string_mutation_find(run, "scenario"), NULL, 0);
        const bool started = status == XR_XIR_CALL_READY;
        while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(first, UINT64_MAX).outcome.status;
        runtime_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_CALL_OOM && runtime_attempts > failure && first->domain == domain);
        /* A driven root preserves its authenticated OOM completion; a rejected
         * start never replaces the previously consumed result. */
        XrXirValue value = {0};
        CHECK(xr_xir_instance_take_result(first, &value) == (started ? XR_XIR_CALL_OOM : XR_XIR_CALL_BAD_STATE));
        CHECK(!value.type && !value.reserved && !value.payload);
        const XrXirDomainBudgetStats charged = xr_xir_domain_budget_stats(domain);
        CHECK(charged.work > before.work && charged.requested_bytes >= before.requested_bytes &&
            charged.requested_call_bytes >= before.requested_call_bytes);
        string_mutation_answer(run, second); string_mutation_answer(run, first);
        const XrXirDomainBudgetStats retried = xr_xir_domain_budget_stats(domain);
        CHECK(retried.work > charged.work && retried.requested_bytes >= charged.requested_bytes &&
            retried.requested_call_bytes > charged.requested_call_bytes);
        CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes);
    }
    printf("string-mutation mode%u runtime OOM sites=%zu retries same Domain/Instance dual physical=0/0\n", mode, sites);
}
static void string_mutation_cancellation(CapacityCompile *run, unsigned mode) {
    XrXirInstance *first = string_mutation_open(run); const uint32_t function = string_mutation_find(run, "scenario");
    CHECK(xr_xir_instance_start(first, function, NULL, 0) == XR_XIR_CALL_READY);
    size_t ticks = 0; XrXirCallStatus status = XR_XIR_CALL_READY;
    while (status == XR_XIR_CALL_READY) { CHECK(++ticks < 4096); status = xr_xir_instance_poll_bounded(first, 1).outcome.status; }
    CHECK(status == XR_XIR_CALL_RETURNED); XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(first, &value) == XR_XIR_CALL_RETURNED && value.payload == 66);
    xr_xir_value_drop(&value); CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);
    for (size_t prefix = 0; prefix < ticks; ++prefix) {
        first = string_mutation_open(run); XrXirInstance *second = string_mutation_open(run);
        CHECK(xr_xir_instance_start(first, function, NULL, 0) == XR_XIR_CALL_READY);
        for (size_t i = 0; i < prefix; ++i) CHECK(xr_xir_instance_poll_bounded(first, 1).outcome.status == XR_XIR_CALL_READY);
        runtime_attempts = 0; CHECK(xr_xir_instance_cancel_current(first) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(first, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED && !runtime_attempts);
        value = (XrXirValue){0}; CHECK(xr_xir_instance_take_result(first, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
        string_mutation_answer(run, second); string_mutation_answer(run, first);
        CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes);
    }
    printf("string-mutation mode%u cancel prefixes=%zu late allocations=0 dual physical=0/0\n", mode, ticks);
}
static void string_mutation_escape(CapacityCompile *run) {
    XrXirInstance *first = string_mutation_open(run), *second = string_mutation_open(run);
    const uint32_t function = string_mutation_find(run, "escaped");
    XrXirValue values[] = {string_mutation_result(first, function), string_mutation_result(second, function)};
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program); run->program = NULL; run->module = NULL;
    CHECK(runtime_live && runtime_bytes);
    for (unsigned n = 0; n < 2; ++n) {
        const char *bytes = NULL; size_t length = 0;
        CHECK(values[n].type == XR_XIR_STRING && xr_xir_string_view(&values[n], &bytes, &length));
        CHECK(length == 5 && !memcmp(bytes, "ab\0\xce\xa9", 5) && bytes[length] == 0);

    }
    xr_xir_value_drop(&values[1]); CHECK(runtime_live && runtime_bytes);
    xr_xir_value_drop(&values[0]); CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--compiler")) {
        unsigned mode = (unsigned)strtoul(argv[2], NULL, 10); CHECK(mode < 4);
        string_mutation_compiler(mode); instance_compile_report(); return 0;
    }
    CHECK(argc == 2 || argc == 3); unsigned mode = (unsigned)strtoul(argv[1], NULL, 10); CHECK(mode < 4);
    bool normal = argc == 3 && !strcmp(argv[2], "--normal");
    CapacityCompile run = capacity_build(SIZE_MAX, capacity_limits(), mode, argc == 3 && !normal ? argv[2] : NULL);
    printf("string-mutation compiler status%u stage%u sites%zu allocated%llu peak%llu work%llu\n", run.status, run.stage, run.sites,
        (unsigned long long)run.stats.allocated_bytes, (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work);
    CHECK(run.status == XR_XIR_OK);
    if (normal) {
        XrXirInstance *first = string_mutation_open(&run), *second = string_mutation_open(&run);
        string_mutation_answer(&run, first); string_mutation_answer(&run, second);
        CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes); string_mutation_escape(&run);
        puts("String mutation result=66 independent COW, dual Instance and final owner physical=0/0");
    } else if (argc == 2) { string_mutation_failures(&run, mode); string_mutation_cancellation(&run, mode); string_mutation_escape(&run); }
    capacity_release(&run); instance_compile_report(); return 0;
}
