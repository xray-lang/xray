/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_deep_source_compiler.c - Complete observed Source image preparation failures
 */
#define XR_DEEP_SOURCE_PREPARATION_MAIN
#include "test_xir_mixed_deep_source_lifetime.c"

static unsigned deep_source_number(const char *text) {
    char *end = NULL; unsigned long value = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && value < 20000); return (unsigned)value;
}
static DeepCompileResult deep_source_compile_operation(unsigned backend, size_t failure, XrCompileResourceLimits caps) {
    DeepSourcePrepared prepared = deep_source_prepare(backend, failure, caps);
    if (prepared.program) {
        CHECK(prepared.compile.status == XR_XIR_OK && prepared.compile.stage == 13 && !prepared.compile.injected);
        xr_xir_compile_program_drop(prepared.program);
        CHECK(worker_code_releases == 1 && !deep_code && !worker_control_code); worker_code_releases = 0;
    } else CHECK(prepared.compile.status != XR_XIR_OK && !worker_code_releases && !deep_code && !worker_control_code);
    CHECK(effects_compile_attempts == prepared.compile.sites);
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return prepared.compile;
}
static void deep_source_compile_axes(unsigned backend, const DeepCompileResult *normal) {
    const uint64_t exact[] = {normal->stats.allocated_bytes, normal->stats.peak_bytes, normal->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        CHECK(exact[axis] > 1); XrCompileResourceLimits caps = deep_compile_caps();
        if (!axis) caps.allocated_bytes = exact[axis]-minus;
        else if (axis == 1) caps.live_bytes = exact[axis]-minus;
        else caps.work = exact[axis]-minus;
        DeepCompileResult run = deep_source_compile_operation(backend, SIZE_MAX, caps);
        printf("Deep Source compiler axis backend=%s axis=%u minus=%u status=%u stage=%u allocated=%llu peak=%llu work=%llu\n",
            worker_backends[backend], axis, minus, run.status, run.stage, (unsigned long long)run.stats.allocated_bytes,
            (unsigned long long)run.stats.peak_bytes, (unsigned long long)run.stats.work);
        if (run.status != (minus ? XR_XIR_BUDGET : XR_XIR_OK)) fprintf(stderr, "Deep Source compiler axis=%u minus=%u stage=%u status=%u\n", axis, minus, run.stage, run.status);
        CHECK(run.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(run.stats.allocated_bytes <= caps.allocated_bytes && run.stats.peak_bytes <= caps.live_bytes && run.stats.work <= caps.work);
    }
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 6); (void)deep_source_lifetime_main;
    unsigned backend = 0; while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend; CHECK(backend < 4);
    DeepCompileResult normal = deep_source_compile_operation(backend, SIZE_MAX, deep_compile_caps());
    printf("Deep Source compiler backend=%s status=%u stage=%u sites=%zu allocated=%llu peak=%llu work=%llu cbytes=%zu, oneFiniteOwner callerReferenceDead observedImageRetired physical0/0\n",
        worker_backends[backend], normal.status, normal.stage, normal.sites, (unsigned long long)normal.stats.allocated_bytes,
        (unsigned long long)normal.stats.peak_bytes, (unsigned long long)normal.stats.work, normal.c_bytes); fflush(stdout);
    CHECK(normal.status == XR_XIR_OK && normal.stage == 13 && normal.sites && normal.sites < 20000);
    if (!strcmp(argv[2], "baseline")) { CHECK(argc == 3); return 0; }
    if (!strcmp(argv[2], "axes")) { CHECK(argc == 3); deep_source_compile_axes(backend, &normal); return 0; }
    CHECK(argc == 6 && !strcmp(argv[2], "fi"));
    size_t begin = deep_source_number(argv[3]), end = deep_source_number(argv[4]), expected = deep_source_number(argv[5]);
    CHECK(begin < end && end <= expected && normal.sites == expected);
    for (size_t site = begin; site < end; ++site) {
        DeepCompileResult run = deep_source_compile_operation(backend, site, deep_compile_caps());
        if (run.status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr, "Deep Source compiler FI backend=%s site=%zu stage=%u status=%u hit=%u attempts=%zu\n",
            worker_backends[backend], site, run.stage, run.status, (unsigned)run.injected, run.sites);
        CHECK(run.status == XR_XIR_OUT_OF_MEMORY && run.injected && run.sites > site);
    }
    printf("Deep Source compiler complete FI backend=%s range=%zu:%zu of%zu allActualOrdinal typedOOM physical0/0\n", worker_backends[backend], begin, end, expected);
    return 0;
}
