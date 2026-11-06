/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_resize_compiler.h - Whole-operation physical failures and held memory
 *
 * KEY CONCEPT:
 *   Every attempt starts with a fresh owner and replays the complete program.
 */
#ifndef XIR_ARRAY_RESIZE_COMPILER_H
#define XIR_ARRAY_RESIZE_COMPILER_H
#include <limits.h>
static void resize_compiler(unsigned mode) {
    ResizeCompile baseline = resize_build(SIZE_MAX, resize_limits(), mode, NULL);
    CHECK(baseline.status == XR_XIR_OK && baseline.program && baseline.sites > 0);
    const size_t sites = baseline.sites;
    const XrCompileResourceStats stats = baseline.stats;
#if defined(XR_RESIZE_NATIVE)
    XrXirProgram *occupied = baseline.program;
    CHECK(xr_xir_compile_program_seal(&baseline.context, &array_resize_program, &occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == baseline.program);
    XrCompileResourceStats after = resize_stats(&baseline.context);
    CHECK(after.allocated_bytes == stats.allocated_bytes && after.held_bytes == stats.held_bytes);
#endif
    printf("compiler mode%u baseline-sites%zu allocated%llu peak%llu\n", mode, sites,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes);
    fflush(stdout);
    const clock_t started = clock();
    resize_release(&baseline);
    const size_t faults[3] = {0, sites / 2, sites - 1};
    for (unsigned point = 0; point < 3; ++point) {
        const size_t failure = faults[point];
        if (point && failure == faults[point - 1]) continue;
        ResizeCompile rejected = resize_build(failure, resize_limits(), mode, NULL);
        if (rejected.status != XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr, "compiler ordinal%zu/%zu stage%u status%u attempts%zu\n", failure, sites, rejected.stage, rejected.status, rejected.sites);
        CHECK(rejected.status == XR_XIR_OUT_OF_MEMORY && !rejected.program && !rejected.lowered);
        CHECK(instance_compile_injected && rejected.sites > failure); resize_release(&rejected);
        if ((failure + 1) % 128 == 0 || failure + 1 == sites) {
            printf("compiler mode%u representative-ordinal%zu/%zu seconds%.6f physical0\n", mode, failure + 1, sites,
                (double)(clock() - started) / CLOCKS_PER_SEC); fflush(stdout);
        }
    }
    const uint64_t exact = stats.peak_bytes;
    CHECK(exact > 1);
    for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        XrCompileResourceLimits limits = resize_limits();
        limits.memory_limit = exact - shortfall;
        ResizeCompile run = resize_build(SIZE_MAX, limits, mode, NULL);
        CHECK(run.status == (shortfall ? XR_XIR_MEMORY_LIMIT : XR_XIR_OK));
        CHECK(shortfall ? !run.program && !run.lowered : run.program != NULL);
        CHECK(run.stats.peak_bytes <= limits.memory_limit);
        resize_release(&run);
    }
    printf("resize compiler mode%u observed-sites%zu representative-OOM allocated%llu peak%llu held-exact-minus physical0\n", mode, sites,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes);
}

static void resize_physical_record(FILE *file) {
    CHECK(!instance_compile_live && !instance_compile_bytes && !runtime_live && !runtime_bytes);
    CHECK(fprintf(file, "\"compiler_blocks\":%zu,\"compiler_bytes\":%zu,\"runtime_blocks\":%zu,\"runtime_bytes\":%zu}",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes) > 0);
    CHECK(fputc('\n', file) != EOF && fflush(file) == 0);
}
static void resize_parallel_baseline(FILE *file, unsigned mode, size_t *sites, XrCompileResourceStats *stats) {
    resize_compiler_digest = true;
    ResizeCompile run = resize_build(SIZE_MAX, resize_limits(), mode, NULL);
    CHECK(run.status == XR_XIR_OK && run.program && run.sites);
    resize_compiler_digest = false;
    *sites = run.sites; *stats = run.stats;
    const uint32_t functions = run.module->function_count;
    const size_t bytes = run.c_bytes;
#if defined(XR_RESIZE_NATIVE)
    XrXirProgram *occupied = run.program;
    CHECK(xr_xir_compile_program_seal(&run.context, &array_resize_program, &occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == run.program);
    XrCompileResourceStats after = resize_stats(&run.context);
    CHECK(after.allocated_bytes == stats->allocated_bytes && after.held_bytes == stats->held_bytes);
#endif
    CHECK(fprintf(file, "{\"kind\":\"generated_identity\",\"c_sha256\":\"%s\",\"proof_sha256\":\"%s\"}\n",
        run.c_sha, run.proof_sha) > 0);
    resize_release(&run);
    CHECK(fprintf(file, "{\"kind\":\"baseline\",\"mode\":%u,\"sites\":%zu,\"functions\":%u,\"c_bytes\":%zu,"
        "\"allocated\":%llu,\"peak\":%llu,\"memory_limit\":8388608,\"oom_status\":%u,", mode, *sites, functions, bytes,
        (unsigned long long)stats->allocated_bytes, (unsigned long long)stats->peak_bytes, XR_XIR_OUT_OF_MEMORY) > 0);
    resize_physical_record(file);
}
static void resize_parallel_bounds(FILE *file, unsigned mode, const XrCompileResourceStats *stats) {
    const uint64_t exact = stats->peak_bytes;
    CHECK(exact > 1);
    for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        XrCompileResourceLimits limits = resize_limits();
        limits.memory_limit = exact - shortfall;
        ResizeCompile run = resize_build(SIZE_MAX, limits, mode, NULL);
        CHECK(run.status == (shortfall ? XR_XIR_MEMORY_LIMIT : XR_XIR_OK));
        CHECK(shortfall ? !run.program && !run.lowered : run.program != NULL);
        CHECK(run.stats.peak_bytes <= limits.memory_limit);
        const XrXirStatus status = run.status;
        resize_release(&run);
        CHECK(fprintf(file, "{\"kind\":\"memory_bound\",\"mode\":%u,\"shortfall\":%u,\"status\":%u,",
            mode, shortfall, status) > 0);
        resize_physical_record(file);
    }
}
static unsigned resize_parallel_argument(const char *text, unsigned maximum) {
    CHECK(text && *text);
    unsigned result = 0;
    while (*text) {
        CHECK(*text >= '0' && *text <= '9');
        unsigned digit = (unsigned)(*text++ - '0');
        CHECK(digit <= maximum && result <= (maximum - digit) / 10);
        result = result * 10 + digit;
    }
    return result;
}
static int resize_parallel_cli(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "--compiler-baseline") && strcmp(argv[1], "--compiler-bounds") &&
                    strcmp(argv[1], "--compiler-shard"))) return 0;
    const bool shard = !strcmp(argv[1], "--compiler-shard");
    CHECK(argc == (shard ? 7 : 4));
    const unsigned mode = resize_parallel_argument(argv[2], 3);
    const unsigned index = shard ? resize_parallel_argument(argv[3], 7) : 0;
    const unsigned workers = shard ? resize_parallel_argument(argv[4], 8) : 1;
    const unsigned expected = shard ? resize_parallel_argument(argv[5], UINT_MAX) : 0;
    CHECK(workers && index < workers && (!shard || expected));
    FILE *file = fopen(argv[argc - 1], "wb"); CHECK(file);
    size_t sites = 0; XrCompileResourceStats stats = {0};
    resize_parallel_baseline(file, mode, &sites, &stats);
    if (shard) {
        CHECK(sites == expected);
        const size_t faults[3] = {0, sites / 2, sites - 1};
        for (unsigned point = 0; point < 3; ++point) {
            const size_t failure = faults[point];
            if ((point && failure == faults[point - 1]) || failure % workers != index) continue;
            ResizeCompile run = resize_build(failure, resize_limits(), mode, NULL);
            CHECK(run.status == XR_XIR_OUT_OF_MEMORY && !run.program && !run.lowered);
            CHECK(instance_compile_injected && run.sites > failure);
            const size_t attempts = run.sites;
            const unsigned stage = run.stage;
            resize_release(&run);
            CHECK(fprintf(file, "{\"kind\":\"point\",\"mode\":%u,\"ordinal\":%zu,\"attempts\":%zu,\"stage\":%u,"
                "\"status\":%u,\"injected\":true,\"empty_output\":true,", mode, failure, attempts, stage, XR_XIR_OUT_OF_MEMORY) > 0);
            resize_physical_record(file);
        }
    } else if (!strcmp(argv[1], "--compiler-bounds")) resize_parallel_bounds(file, mode, &stats);
    CHECK(fprintf(file, "{\"kind\":\"complete\",\"mode\":%u,\"index\":%u,\"workers\":%u,\"sites\":%zu,",
        mode, index, workers, sites) > 0);
    resize_physical_record(file); CHECK(fclose(file) == 0); return 1;
}

#endif // XIR_ARRAY_RESIZE_COMPILER_H
