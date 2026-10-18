/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_string_append_probe.c - Source append accounting before replacement
 *
 * KEY CONCEPT:
 *   Unchanged execution bounds distinguish a measured limit from completion.
 */
#define main original_reference_main
#include "p09_reference/test_value_reference.c"
#undef main
#include <time.h>

static uint64_t probe_copy_bytes, probe_string_copy_bytes;
static XrXirDomain *probe_domain;
static bool probe_string_copy(const void *destination, const void *source, size_t bytes);
static void *probe_memcpy(void *destination, const void *source, size_t bytes) {
    CHECK(bytes <= UINT64_MAX - probe_copy_bytes); probe_copy_bytes += bytes;
    if (probe_domain && probe_string_copy(destination, source, bytes)) probe_string_copy_bytes += bytes;
    return memcpy(destination, source, bytes);
}
static void *probe_memmove(void *destination, const void *source, size_t bytes) {
    CHECK(bytes <= UINT64_MAX - probe_copy_bytes); probe_copy_bytes += bytes;
    if (probe_domain && probe_string_copy(destination, source, bytes)) probe_string_copy_bytes += bytes;
    return memmove(destination, source, bytes);
}
#define memcpy(destination, source, bytes) probe_memcpy(destination, source, bytes)
#define memmove(destination, source, bytes) probe_memmove(destination, source, bytes)
#include "xir_runtime_allocations.h"
#undef memmove
#undef memcpy

static bool probe_string_copy(const void *destination, const void *source, size_t bytes) {
    uintptr_t addresses[] = {(uintptr_t)destination, (uintptr_t)source};
    for (XirObject *object = probe_domain->objects; object; object = object->graph_next) {
        if (object->type != XR_XIR_STRING || object->kind) continue;
        XirString *string = (XirString *)object;
        if (bytes > string->capacity) continue;
        uintptr_t base = (uintptr_t)string->bytes;
        for (unsigned side = 0; side < 2; ++side)
            if (addresses[side] >= base && addresses[side] - base <= string->capacity - bytes) return true;
    }
    return false;
}

static void append_probe_run(XrXirProgram *program, uint32_t entry, uint64_t count) {
    XrXirInstanceConfig config; XrXirInstance *instance = NULL;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
#ifdef XR_P09_APPEND_PERFORMANCE_POLICY
    /* This separate benchmark covers a known finite loop. Original default
     * and resource gates retain their original policy and all byte/work caps. */
    config.poll_limit = count * UINT64_C(64) + UINT64_C(4096);
#endif
    CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    runtime_attempts = 0; probe_copy_bytes = 0; probe_string_copy_bytes = 0; probe_domain = instance->domain;
    clock_t started = clock();
    XrXirCallStatus status = xr_xir_instance_start(instance, entry, NULL, 0);
    while (status == XR_XIR_CALL_READY)
        status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
    const double seconds = (double)(clock() - started) / CLOCKS_PER_SEC;
    const XrXirDomainStats storage = xr_xir_domain_stats(instance->domain);
    const XrXirDomainBudgetStats budget = xr_xir_domain_budget_stats(instance->domain);
#ifdef XR_P09_APPEND_PERFORMANCE_POLICY
    CHECK(status == XR_XIR_CALL_RETURNED);
    CHECK(probe_string_copy_bytes <= count * 2 + 64 && runtime_attempts <= 64);
    CHECK(storage.peak_bytes <= count * 2 + 4096 && budget.requested_bytes <= count * 4 + 4096);
    CHECK(budget.work <= count * 64 + 4096);
#endif
    XrXirValue result = {0};
    if (status == XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance, &result) == status &&
            result.type == XR_XIR_I64 && result.payload == (int64_t)count);
    } else {
        CHECK(status == XR_XIR_CALL_LIMIT);
        CHECK(xr_xir_instance_take_result(instance, &result) == status &&
            !result.type && !result.reserved && !result.payload);
    }
    printf("{\"append_count\":%llu,\"outcome\":%u,\"complete\":%s,\"result\":%lld,"
        "\"runtime_allocations\":%zu,\"runtime_copy_bytes\":%llu,\"string_copy_bytes\":%llu,\"live\":%llu,\"peak\":%llu,"
        "\"requested_values\":%llu,\"work\":%llu,\"reallocations\":%llu,\"cpu_seconds\":%.6f,"
        "\"requested_limit\":%llu,\"poll_limit\":%llu,\"work_limit\":%llu}\n",
        (unsigned long long)count, status, status == XR_XIR_CALL_RETURNED ? "true" : "false",
        (long long)result.payload, runtime_attempts, (unsigned long long)probe_copy_bytes,
        (unsigned long long)probe_string_copy_bytes,
        (unsigned long long)storage.live_bytes, (unsigned long long)storage.peak_bytes,
        (unsigned long long)budget.requested_bytes, (unsigned long long)budget.work,
        (unsigned long long)storage.reallocations, seconds,
        (unsigned long long)config.requested_value_limit, (unsigned long long)config.poll_limit,
        (unsigned long long)config.work_limit);
    xr_xir_value_drop(&result);
    probe_domain = NULL;
    XrXirCallStatus released = xr_xir_instance_free(instance);
    CHECK(released == (status == XR_XIR_CALL_LIMIT ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY));
    printf("append probe release_status=%u (original executor completion), ", released);
    CHECK(!runtime_live && !runtime_bytes);
    puts("append probe actual runtime physical=0/0; complete=false is not a performance PASS");
}
#ifndef XR_P09_APPEND_PROBE_ENTRY
#define XR_P09_APPEND_PROBE_ENTRY main
#endif
int XR_P09_APPEND_PROBE_ENTRY(int argc, char **argv) {
    CHECK(argc == 2); char *end = NULL;
    uint64_t count = (uint64_t)strtoull(argv[1], &end, 10);
    CHECK(end && !*end && count && count <= 1000000);
    char path[1024];
    int bytes = snprintf(path, sizeof(path), XR_REFERENCE_FIXTURES "/string_append_%llu.xr",
        (unsigned long long)count);
    CHECK(bytes > 0 && (size_t)bytes < sizeof(path));
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(owner.context.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_REFERENCE_FIXTURES};
    XrXirSourceRequest request = {session, path, &authority, &owner.context,
        XR_REFERENCE_STDLIB, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
    CHECK(xr_xir_compile_source_check(&request, &result, &diagnostic, NULL) == XR_XIR_OK);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(result.checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_source_result_free(&result); xr_compile_session_free(session);
    XrXirArtifact *checked = NULL;
    CHECK(xr_xir_compile_checked_read(&owner.context, packet.bytes, packet.length, &checked, NULL) == XR_XIR_OK);
    xr_xir_compile_checked_packet_free(&packet);
    XrXirArtifact *lowered = reference_lower(checked); uint32_t entry = reference_entry(lowered);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered, &program) == XR_XIR_OK);
    append_probe_run(program, entry, count);
    xr_xir_compile_program_drop(program); source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes); return 0;
}
