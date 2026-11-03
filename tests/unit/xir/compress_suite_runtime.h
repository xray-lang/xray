/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * compress_suite_runtime.h - Execute authentic compression test entries
 *
 * KEY CONCEPT:
 *   Test-role admission starts every discovered function in a fresh Instance.
 *   Reports distinguish emitted metadata from actual terminal Unit results.
 */
#ifndef COMPRESS_SUITE_RUNTIME_H
#define COMPRESS_SUITE_RUNTIME_H

#include "xir/xxir_program.h"
#include "compress_suite_inventory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMPRESS_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

typedef struct CompressSuiteDigests {
    const char *source_sha256, *emit_sha256;
} CompressSuiteDigests;

static XrXirCallStatus compress_suite_run(XrXirProgram *program, uint32_t function,
    const char *backend, const char *name) {
    XrXirInstanceConfig config;
    COMPRESS_CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    XrXirInstance *instance = NULL;
    COMPRESS_CHECK(xr_xir_instance_new(program, &config, &instance) == XR_XIR_CALL_READY);
    COMPRESS_CHECK(xr_xir_instance_start(instance, function, NULL, 0) == XR_XIR_CALL_BAD_ARGUMENT);
    COMPRESS_CHECK(xr_xir_instance_start_test(instance, function) == XR_XIR_CALL_READY);
    XrXirInstanceResult result;
    unsigned slices = 0;
    do {
        COMPRESS_CHECK(++slices <= 1000000u);
        result = xr_xir_instance_poll_bounded(instance, 1024);
    } while (result.outcome.status == XR_XIR_CALL_READY);
    bool passed = result.outcome.status == XR_XIR_CALL_RETURNED;
    if (passed) {
        XrXirValue value = {0};
        COMPRESS_CHECK(xr_xir_instance_take_result(instance, &value) == XR_XIR_CALL_RETURNED);
        COMPRESS_CHECK(value.type == XR_XIR_UNIT);
        xr_xir_value_drop(&value);
    } else {
        fprintf(stderr, "%s %s status=%u panic=%u\n", backend, name,
            result.outcome.status, result.outcome.panic.detail.code);
    }
    XrXirCallStatus closed = xr_xir_instance_free(instance);
    COMPRESS_CHECK(closed == XR_XIR_CALL_READY || (!passed && closed == result.outcome.status));
    printf("%s %s %s\n", backend, name, passed ? "PASS" : "FAIL");
    fflush(stdout);
    return result.outcome.status;
}

static void compress_suite_report(const char *path, const char *backend, const CompressSuiteDigests *digests,
    const uint32_t functions[COMPRESS_SUITE_COUNT],
    const XrXirCallStatus statuses[COMPRESS_SUITE_COUNT], bool executed) {
    const char *values[] = {digests->source_sha256, digests->emit_sha256};
    for (unsigned d = 0; d < 2; ++d) {
        COMPRESS_CHECK(strlen(values[d]) == 64);
        for (unsigned i = 0; i < 64; ++i) {
            COMPRESS_CHECK((values[d][i] >= '0' && values[d][i] <= '9') ||
                (values[d][i] >= 'a' && values[d][i] <= 'f'));
        }
    }
    FILE *report = fopen(path, "wb");
    COMPRESS_CHECK(report);
    COMPRESS_CHECK(fprintf(report, "{\"schema\":1,\"backend\":\"%s\",\"source\":\"%s\","
        "\"sourceSha256\":\"%s\",\"emitBodySha256\":\"%s\",\"entries\":[",
        backend, COMPRESS_SUITE_SOURCE, digests->source_sha256, digests->emit_sha256) > 0);
    uint32_t passed = 0;
    for (uint32_t i = 0; i < COMPRESS_SUITE_COUNT; ++i) {
        bool success = executed && statuses[i] == XR_XIR_CALL_RETURNED;
        if (success) ++passed;
        COMPRESS_CHECK(fprintf(report, "%s{\"name\":\"%s\",\"function\":%u,\"role\":%u,"
            "\"timeout\":0,\"executed\":%s,\"passed\":%s,\"unitResult\":%s,\"terminalStatus\":",
            i ? "," : "", compress_suite_names[i], functions[i], XR_XIR_TEST_ROLE_TEST,
            executed ? "true" : "false", success ? "true" : "false", success ? "true" : "false") > 0);
        if (executed) COMPRESS_CHECK(fprintf(report, "%u}", statuses[i]) > 0);
        else COMPRESS_CHECK(fputs("null}", report) >= 0);
    }
    uint32_t count = executed ? COMPRESS_SUITE_COUNT : 0;
    COMPRESS_CHECK(fprintf(report, "],\"discovered\":%u,\"executed\":%u,\"passed\":%u,"
        "\"skipped\":0,\"failed\":%u}\n", COMPRESS_SUITE_COUNT, count, passed, count - passed) > 0);
    COMPRESS_CHECK(fclose(report) == 0);
}

#endif // COMPRESS_SUITE_RUNTIME_H
