/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_reserve.c - Reserve copies across real execution boundaries
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
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_array_capacity_pipeline.h"

static uint32_t reserve_find(const CapacityCompile *run, const char *name) {
    for (uint32_t f = 0; f < run->module->function_count; ++f) {
        const XrXirFunction *function = &run->module->functions[f];
        if (run->module->declarations->functions[f].module == run->module->declarations->root_module &&
            function->name_length == strlen(name) && !memcmp(function->name, name, strlen(name))) return f;
    }
    CHECK(false); return UINT32_MAX;
}
static XrXirCallStatus reserve_poll(XrXirInstance *instance, uint32_t function) {
    XrXirCallStatus status = xr_xir_instance_start(instance, function, NULL, 0);
    while (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    return status;
}
static XrXirValue reserve_result(XrXirInstance *instance, uint32_t function) {
    CHECK(reserve_poll(instance, function) == XR_XIR_CALL_RETURNED);
    XrXirValue value = {0}; CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
    return value;
}
static XrXirInstance *reserve_open(CapacityCompile *run) {
    XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = 1048576; XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue value = reserve_result(instance, run->program->declarations->entry_function);
    CHECK(value.type == XR_XIR_I64 && !value.payload); xr_xir_value_drop(&value); return instance;
}
static void reserve_answer(CapacityCompile *run, XrXirInstance *instance) {
    XrXirValue value = reserve_result(instance, reserve_find(run, "scenario"));
    CHECK(value.type == XR_XIR_I64 && value.payload == 3479); xr_xir_value_drop(&value);
}
static void reserve_failures(CapacityCompile *run, unsigned mode) {
    XrXirInstance *first = reserve_open(run), *second = reserve_open(run);
    runtime_attempts = 0; reserve_answer(run, first); const size_t sites = runtime_attempts; CHECK(sites);
    reserve_answer(run, second);
    CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);
    for (size_t failure = 0; failure < sites; ++failure) {
        first = reserve_open(run); second = reserve_open(run);
        XrXirDomain *domain = first->domain; const XrXirDomainBudgetStats before = xr_xir_domain_budget_stats(domain);
        runtime_attempts = 0; runtime_fail_at = failure;
        XrXirCallStatus status = xr_xir_instance_start(first, reserve_find(run, "scenario"), NULL, 0);
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
        reserve_answer(run, second); reserve_answer(run, first);
        const XrXirDomainBudgetStats retried = xr_xir_domain_budget_stats(domain);
        CHECK(retried.work > charged.work && retried.requested_bytes >= charged.requested_bytes &&
            retried.requested_call_bytes > charged.requested_call_bytes);
        CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes);
    }
    printf("reserve mode%u runtime OOM sites=%zu retries same Domain/Instance dual physical=0/0\n", mode, sites);
}
static void reserve_cancellation(CapacityCompile *run, unsigned mode) {
    XrXirInstance *first = reserve_open(run); const uint32_t function = reserve_find(run, "scenario");
    CHECK(xr_xir_instance_start(first, function, NULL, 0) == XR_XIR_CALL_READY);
    size_t ticks = 0; XrXirCallStatus status = XR_XIR_CALL_READY;
    while (status == XR_XIR_CALL_READY) { CHECK(++ticks < 4096); status = xr_xir_instance_poll_bounded(first, 1).outcome.status; }
    CHECK(status == XR_XIR_CALL_RETURNED); XrXirValue value = {0};
    CHECK(xr_xir_instance_take_result(first, &value) == XR_XIR_CALL_RETURNED && value.payload == 3479);
    xr_xir_value_drop(&value); CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY);
    CHECK(!runtime_live && !runtime_bytes);
    for (size_t prefix = 0; prefix < ticks; ++prefix) {
        first = reserve_open(run); XrXirInstance *second = reserve_open(run);
        CHECK(xr_xir_instance_start(first, function, NULL, 0) == XR_XIR_CALL_READY);
        for (size_t i = 0; i < prefix; ++i) CHECK(xr_xir_instance_poll_bounded(first, 1).outcome.status == XR_XIR_CALL_READY);
        runtime_attempts = 0; CHECK(xr_xir_instance_cancel_current(first) == XR_XIR_CALL_CANCEL_REQUESTED);
        CHECK(xr_xir_instance_poll_bounded(first, UINT64_MAX).outcome.status == XR_XIR_CALL_CANCELLED && !runtime_attempts);
        value = (XrXirValue){0}; CHECK(xr_xir_instance_take_result(first, &value) == XR_XIR_CALL_BAD_STATE && !value.type && !value.payload);
        reserve_answer(run, second); reserve_answer(run, first);
        CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
        CHECK(!runtime_live && !runtime_bytes);
    }
    printf("reserve mode%u cancel prefixes=%zu late allocations=0 dual physical=0/0\n", mode, ticks);
}
static void reserve_escape(CapacityCompile *run) {
    XrXirInstance *first = reserve_open(run), *second = reserve_open(run);
    const uint32_t function = reserve_find(run, "escaped");
    XrXirValue values[] = {reserve_result(first, function), reserve_result(second, function)};
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    CHECK(xr_xir_instance_free(first) == XR_XIR_CALL_READY && xr_xir_instance_free(second) == XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program); run->program = NULL; run->module = NULL;
    CHECK(runtime_live && runtime_bytes);
    for (unsigned n = 0; n < 2; ++n) {
        XrXirValueAdmission admission = {xr_xir_value_arena(&values[n]), object_pointer(&values[n])->domain,
            NULL, NULL, 100000, 1048576};
        int64_t length = -1, capacity = -1;
        CHECK(xr_xir_array_len(&values[n], &admission, &length) == XR_XIR_VALUE_OK && length == 2);
        CHECK(xr_xir_array_capacity(&values[n], &admission, &capacity) == XR_XIR_VALUE_OK && capacity >= 8);
        for (int64_t i = 0; i < 2; ++i) {
            XrXirValue item = {0}; XrXirFaultDetail fault = {0};
            CHECK(xr_xir_array_get(&values[n], i, &admission, &item, &fault) == XR_XIR_VALUE_OK);
            CHECK(item.type == XR_XIR_I64 && item.payload == 3 + i); xr_xir_value_drop(&item);
        }
    }
    xr_xir_value_drop(&values[1]); CHECK(runtime_live && runtime_bytes);
    xr_xir_value_drop(&values[0]); CHECK(!runtime_live && !runtime_bytes);
}
int main(int argc, char **argv) {
    CHECK(argc == 2 || argc == 3); unsigned mode = (unsigned)strtoul(argv[1], NULL, 10); CHECK(mode < 4);
    CapacityCompile run = capacity_build(SIZE_MAX, capacity_limits(), mode, argc == 3 ? argv[2] : NULL);
    printf("reserve compiler status%u stage%u sites%zu allocated%llu peak%llu work%llu\n", run.status, run.stage, run.sites,
        (unsigned long long)run.stats.allocated_bytes, (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work);
    CHECK(run.status == XR_XIR_OK);
    if (argc == 2) { reserve_failures(&run, mode); reserve_cancellation(&run, mode); reserve_escape(&run); }
    capacity_release(&run); instance_compile_report(); return 0;
}
