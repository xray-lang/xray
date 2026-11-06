/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_capacity_compiler.h - Whole-operation physical failures and held memory
 *
 * KEY CONCEPT:
 *   Every attempt starts with a fresh owner and replays the complete program.
 */
#ifndef XIR_ARRAY_CAPACITY_COMPILER_H
#define XIR_ARRAY_CAPACITY_COMPILER_H
static void capacity_compiler(unsigned mode) {
    CapacityCompile baseline = capacity_build(SIZE_MAX, capacity_limits(), mode, NULL);
    CHECK(baseline.status == XR_XIR_OK && baseline.program && baseline.sites > 0);
    const size_t sites = baseline.sites;
    const XrCompileResourceStats stats = baseline.stats;
#if defined(XR_CAPACITY_NATIVE)
    XrXirProgram *occupied = baseline.program;
    CHECK(xr_xir_compile_program_seal(&baseline.context, &array_capacity_program, &occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == baseline.program);
    XrCompileResourceStats after = capacity_stats(&baseline.context);
    CHECK(after.allocation_count == stats.allocation_count && after.allocated_bytes == stats.allocated_bytes && after.held_bytes == stats.held_bytes);
#endif
    printf("compiler mode%u baseline-sites%zu allocated%llu peak%llu\n", mode, sites,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes);
    fflush(stdout);
    const clock_t started = clock();
    capacity_release(&baseline);
    for (size_t failure = 0; failure < sites; ++failure) {
        CapacityCompile rejected = capacity_build(failure, capacity_limits(), mode, NULL);
        if (rejected.status != XR_XIR_OUT_OF_MEMORY)
            fprintf(stderr, "compiler ordinal%zu/%zu stage%u status%u attempts%zu\n", failure, sites, rejected.stage, rejected.status, rejected.sites);
        CHECK(rejected.status == XR_XIR_OUT_OF_MEMORY && !rejected.program && !rejected.lowered);
        CHECK(instance_compile_injected && rejected.sites > failure); capacity_release(&rejected);
        if ((failure + 1) % 128 == 0 || failure + 1 == sites) {
            printf("compiler mode%u completed%zu/%zu seconds%.6f physical0\n", mode, failure + 1, sites,
                (double)(clock() - started) / CLOCKS_PER_SEC); fflush(stdout);
        }
    }
    for (unsigned shortfall = 0; shortfall < 2; ++shortfall)
        capacity_memory_case(mode, stats.peak_bytes, shortfall != 0);
    printf("capacity compiler mode%u actual-faults%zu allocated%llu peak%llu held-exact-minus physical0\n", mode, sites,
        (unsigned long long)stats.allocated_bytes, (unsigned long long)stats.peak_bytes);
}

static void capacity_physical_record(FILE *file) {
    CHECK(!instance_compile_live && !instance_compile_bytes && !runtime_live && !runtime_bytes);
    CHECK(fprintf(file, "\"compiler_blocks\":%zu,\"compiler_bytes\":%zu,\"runtime_blocks\":%zu,\"runtime_bytes\":%zu}",
        instance_compile_live, instance_compile_bytes, runtime_live, runtime_bytes) > 0);
    CHECK(fputc('\n', file) != EOF && fflush(file) == 0);
}
static void capacity_parallel_baseline(FILE *file, unsigned mode, size_t *sites, XrCompileResourceStats *stats, size_t *prefix) {
    capacity_compiler_digest = true;
    CapacityCompile run = capacity_build(SIZE_MAX, capacity_limits(), mode, NULL);
    CHECK(run.status == XR_XIR_OK && run.program && run.sites);
    capacity_compiler_digest = false;
    *sites = run.sites; *stats = run.stats; *prefix = run.prefix_sites;
    const uint32_t functions = run.module->function_count;
    const size_t bytes = run.c_bytes;
#if defined(XR_CAPACITY_NATIVE)
    XrXirProgram *occupied = run.program;
    CHECK(xr_xir_compile_program_seal(&run.context, &array_capacity_program, &occupied) == XR_XIR_BAD_STRUCTURE);
    CHECK(occupied == run.program);
    XrCompileResourceStats after = capacity_stats(&run.context);
    CHECK(after.allocation_count == stats->allocation_count && after.allocated_bytes == stats->allocated_bytes && after.held_bytes == stats->held_bytes);
#endif
    CHECK(fprintf(file, "{\"kind\":\"prefix_identity\",\"prefix_sites\":%zu,\"c_sha256\":\"%s\",\"proof_sha256\":\"%s\","
        "\"allocation_count\":%llu,\"allocated\":%llu,\"held\":%llu,\"peak\":%llu,\"references\":%llu,"
        "\"compiler_blocks\":%zu,\"compiler_bytes\":%zu}\n", run.prefix_sites, run.c_sha, run.proof_sha,
        (unsigned long long)run.prefix_stats.allocation_count, (unsigned long long)run.prefix_stats.allocated_bytes,
        (unsigned long long)run.prefix_stats.held_bytes, (unsigned long long)run.prefix_stats.peak_bytes,
        (unsigned long long)run.prefix_references, run.prefix_blocks, run.prefix_bytes) > 0);
    capacity_release(&run);
    CHECK(fprintf(file, "{\"kind\":\"baseline\",\"mode\":%u,\"sites\":%zu,\"functions\":%u,\"c_bytes\":%zu,"
        "\"allocated\":%llu,\"peak\":%llu,\"memory_mode\":\"ordinary\","
        "\"profile_allocations\":true,\"oom_status\":%u,", mode, *sites, functions, bytes,
        (unsigned long long)stats->allocated_bytes, (unsigned long long)stats->peak_bytes,
        XR_XIR_OUT_OF_MEMORY) > 0);
    capacity_physical_record(file);
}
static void capacity_parallel_bounds(FILE *file, unsigned mode, const XrCompileResourceStats *stats) {
    for (unsigned shortfall = 0; shortfall < 2; ++shortfall) {
        const XrXirStatus status = capacity_memory_case(mode, stats->peak_bytes, shortfall != 0);
        CHECK(fprintf(file, "{\"kind\":\"axis\",\"mode\":%u,\"axis\":0,\"shortfall\":%u,\"status\":%u,",
            mode, shortfall, status) > 0);
        capacity_physical_record(file);
    }
}
static unsigned capacity_parallel_argument(const char *text, unsigned maximum) {
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
static int capacity_parallel_cli(int argc, char **argv) {
    if (argc < 2 || (strcmp(argv[1], "--compiler-baseline") && strcmp(argv[1], "--compiler-bounds") &&
                    strcmp(argv[1], "--compiler-shard"))) return 0;
    const bool shard = !strcmp(argv[1], "--compiler-shard");
    CHECK(argc == (shard ? 7 : 4));
    const unsigned mode = capacity_parallel_argument(argv[2], 3);
    const unsigned index = shard ? capacity_parallel_argument(argv[3], 7) : 0;
    const unsigned workers = shard ? capacity_parallel_argument(argv[4], 8) : 1;
    const unsigned expected = shard ? capacity_parallel_argument(argv[5], UINT32_MAX) : 0;
    CHECK(workers && index < workers && (!shard || expected));
    FILE *file = fopen(argv[argc - 1], "wb"); CHECK(file);
    size_t sites = 0, prefix = 0; XrCompileResourceStats stats = {0};
    capacity_parallel_baseline(file, mode, &sites, &stats, &prefix);
    if (shard) {
        CHECK(sites == expected);
        for (size_t failure = index; failure < sites; failure += workers) {
            CapacityCompile run = capacity_build(failure, capacity_limits(), mode, NULL);
            CHECK(run.status == XR_XIR_OUT_OF_MEMORY && !run.program && !run.lowered);
            CHECK(instance_compile_injected && run.sites > failure);
            const size_t attempts = run.sites;
            const unsigned stage = run.stage;
            capacity_release(&run);
            CHECK(fprintf(file, "{\"kind\":\"point\",\"mode\":%u,\"ordinal\":%zu,\"attempts\":%zu,\"stage\":%u,"
                "\"status\":%u,\"injected\":true,\"empty_output\":true,", mode, failure, attempts, stage, XR_XIR_OUT_OF_MEMORY) > 0);
            capacity_physical_record(file);
        }
    } else if (!strcmp(argv[1], "--compiler-bounds")) capacity_parallel_bounds(file, mode, &stats);
    CHECK(fprintf(file, "{\"kind\":\"complete\",\"mode\":%u,\"index\":%u,\"workers\":%u,\"sites\":%zu,",
        mode, index, workers, sites) > 0);
    capacity_physical_record(file); CHECK(fclose(file) == 0); return 1;
}

#endif // XIR_ARRAY_CAPACITY_COMPILER_H
