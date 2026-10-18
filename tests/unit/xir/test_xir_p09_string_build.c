/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_build.c - Observe complete Source construction results
 *
 * KEY CONCEPT:
 *   The original finite storage policy and actual entry tables remain intact.
 *   Full returned bytes are checked after the execution owners are released.
 */
#include "base/xmalloc.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
static uint64_t build_copy_bytes, build_string_copy_bytes;
static bool build_observe_copy;
static bool build_is_string_copy(const void *destination, const void *source, size_t length);
static void *build_memcpy(void *destination, const void *source, size_t length) {
    if (build_observe_copy) {
        if (length > UINT64_MAX - build_copy_bytes) abort();
        build_copy_bytes += length;
        if (build_is_string_copy(destination, source, length)) build_string_copy_bytes += length;
    }
    return memcpy(destination, source, length);
}
static void *build_memmove(void *destination, const void *source, size_t length) {
    if (build_observe_copy) {
        if (length > UINT64_MAX - build_copy_bytes) abort();
        build_copy_bytes += length;
        if (build_is_string_copy(destination, source, length)) build_string_copy_bytes += length;
    }
    return memmove(destination, source, length);
}
#define memcpy(destination, source, length) build_memcpy(destination, source, length)
#define memmove(destination, source, length) build_memmove(destination, source, length)
#define main original_capacity_main
#include "test_xir_array_capacity.c"
#undef main
#undef memmove
#undef memcpy
static XrXirDomain *build_domain;
static bool build_is_string_copy(const void *destination, const void *source, size_t length) {
    if (!build_domain) return false;
    uintptr_t addresses[] = {(uintptr_t)destination, (uintptr_t)source};
    for (XirObject *object = build_domain->objects; object; object = object->graph_next) {
        if (object->type != XR_XIR_STRING || object->kind) continue;
        XirString *string = (XirString *)object;
        if (length > string->capacity) continue;
        for (unsigned side = 0; side < 2; ++side)
            if (addresses[side] >= (uintptr_t)string->bytes &&
                addresses[side] - (uintptr_t)string->bytes <= string->capacity - length) return true;
    }
    return false;
}
static double build_wall(void) {
    struct timespec value; CHECK(timespec_get(&value, TIME_UTC) == TIME_UTC);
    return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}
static void build_check_string(const XrXirValue *value, size_t length, bool digits) {
    const char *bytes = NULL; size_t actual = 0;
    CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value, &bytes, &actual));
    CHECK(actual == length && bytes[length] == 0);
    for (size_t i = 0; i < length; ++i) CHECK(bytes[i] == (digits ? (char)('0' + i % 10) : 'x'));
}
static void build_check_result(const XrXirValue *value, size_t length, bool digits, bool snapshot) {
    if (!snapshot) { build_check_string(value, length, digits); return; }
    XrXirValue old = {0}, fresh = {0};
    CHECK(xr_xir_tuple_get(value, 0, &old) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(value, 1, &fresh) == XR_XIR_VALUE_OK);
    build_check_string(&old, 4096, false); build_check_string(&fresh, length, false);
    xr_xir_value_drop(&fresh); xr_xir_value_drop(&old);
}
static void build_check_scalars(const XrXirValue *value) {
    const char expected[] = "-17|true|\xce\xa9|1.5|18446744073709551615";
    const char *bytes = NULL; size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && xr_xir_string_view(value, &bytes, &length));
    CHECK(length == sizeof(expected) - 1 && !memcmp(bytes, expected, sizeof(expected)));
}
static void build_scenario(CapacityCompile *run, unsigned mode, const char *scenario) {
    bool digits = !strcmp(scenario, "digits"), chunks = !strcmp(scenario, "chunks");
    bool join = !strcmp(scenario, "join"), snapshot = !strcmp(scenario, "snapshot");
    bool scalars = !strcmp(scenario, "scalars");
    CHECK(digits || chunks || join || snapshot || scalars || !strcmp(scenario, "short"));
    size_t count = chunks ? 64 : join ? 1000 : snapshot ? 4096 : 1000000;
    size_t piece_length = chunks ? 16384 : join ? 1000 : 1;
    size_t length = count * piece_length + (snapshot ? 64 : 0);
    if (scalars) length = 36;
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.poll_limit = UINT64_C(64) * count + 4096;
    XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_new(run->program, &config, &instance) == XR_XIR_CALL_READY);
    XrXirValue initialized = capacity_run(instance, run->program->declarations->entry_function);
    CHECK(initialized.type == XR_XIR_I64 && !initialized.payload); xr_xir_value_drop(&initialized);
    char piece[16384]; memset(piece, 'x', piece_length);
    XrXirValue arguments[] = {{XR_XIR_I64, 0, (int64_t)count}, {0}};
    CHECK(xr_xir_string_new(instance->domain, piece, piece_length, &arguments[1]) == XR_XIR_VALUE_OK);
    uint32_t function = capacity_find(run->module, scenario);
    runtime_attempts = 0; build_copy_bytes = 0; build_string_copy_bytes = 0;
    build_domain = instance->domain; build_observe_copy = true;
    clock_t cpu_start = clock(); double wall_start = build_wall();
    XrXirCallStatus status = xr_xir_instance_start(instance, function, arguments, 2);
    while (status == XR_XIR_CALL_READY)
        status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    double wall = build_wall() - wall_start, cpu = (double)(clock() - cpu_start) / CLOCKS_PER_SEC;
    build_observe_copy = false;
    XrXirValue result = {0};
    CHECK(xr_xir_instance_take_result(instance, &result) == status);
    XrXirDomainStats storage = xr_xir_domain_stats(instance->domain);
    XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(instance->domain);
    if (status == XR_XIR_CALL_RETURNED) {
        if (scalars) build_check_scalars(&result); else build_check_result(&result, length, digits, snapshot);
    }
    else CHECK(status == XR_XIR_CALL_LIMIT && !result.type && !result.reserved && !result.payload);
    size_t allocations = runtime_attempts, physical_live = runtime_bytes;
    xr_xir_value_drop(&arguments[1]);
    CHECK(xr_xir_instance_free(instance) == (status == XR_XIR_CALL_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
    xr_xir_compile_artifact_free(run->lowered); run->lowered = NULL;
    xr_xir_compile_program_drop(run->program); run->program = NULL; run->module = NULL;
    if (status == XR_XIR_CALL_RETURNED) {
        CHECK(runtime_live && runtime_bytes);
        if (scalars) build_check_scalars(&result); else build_check_result(&result, length, digits, snapshot);
    }
    xr_xir_value_drop(&result); build_domain = NULL;
    CHECK(!runtime_live && !runtime_bytes);
    printf("{\"scenario\":\"%s\",\"mode\":%u,\"outcome\":%u,\"complete\":%s,"
        "\"expected_bytes\":%zu,\"runtime_allocation_attempts\":%zu,\"value_requested\":%llu,"
        "\"value_peak\":%llu,\"value_live\":%llu,\"physical_live_before_drop\":%zu,"
        "\"physical_live_after_drop\":%zu,\"explicit_runtime_copy_bytes\":%llu,\"explicit_string_copy_bytes\":%llu,"
        "\"work\":%llu,\"value_reallocations\":%llu,\"poll_limit\":%llu,\"value_limit\":%llu,"
        "\"requested_limit\":%llu,\"work_limit\":%llu,\"wall_seconds\":%.9f,\"cpu_seconds\":%.9f,"
        "\"qualification\":\"CONTENT_AND_MEASUREMENT_ONLY_NOT_REPLACEMENT_PERFORMANCE_PASS\"}\n",
        scenario, mode, status, status == XR_XIR_CALL_RETURNED ? "true" : "false", length,
        allocations, (unsigned long long)budget.requested_bytes, (unsigned long long)storage.peak_bytes,
        (unsigned long long)storage.live_bytes, physical_live, runtime_bytes,
        (unsigned long long)build_copy_bytes, (unsigned long long)build_string_copy_bytes,
        (unsigned long long)budget.work, (unsigned long long)storage.reallocations,
        (unsigned long long)config.poll_limit, (unsigned long long)config.value_limit,
        (unsigned long long)config.requested_value_limit, (unsigned long long)config.work_limit, wall, cpu);
    CHECK(status == XR_XIR_CALL_RETURNED);
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    if (!strcmp(argv[1], "--generate")) {
        CapacityCompile run = capacity_build(SIZE_MAX, capacity_limits(), 0, argv[2]);
        CHECK(run.status == XR_XIR_OK); capacity_release(&run); instance_compile_report(); return 0;
    }
    char *end = NULL; unsigned long mode = strtoul(argv[1], &end, 10);
    CHECK(end && !*end && mode < 4);
    CapacityCompile run = capacity_build(SIZE_MAX, capacity_limits(), (unsigned)mode, NULL);
    if (run.status != XR_XIR_OK)
        fprintf(stderr, "String build compiler status%u stage%u work%llu\n", run.status, run.stage,
            (unsigned long long)run.stats.work);
    CHECK(run.status == XR_XIR_OK);
    build_scenario(&run, (unsigned)mode, argv[2]);
    capacity_release(&run); instance_compile_report(); return 0;
}
