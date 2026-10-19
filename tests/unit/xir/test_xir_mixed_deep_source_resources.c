/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_deep_source_resources.c - Runtime faults after real Source caller retirement
 */
#define XR_DEEP_SOURCE_PREPARATION_MAIN
#include "test_xir_mixed_deep_source_lifetime.c"
#include "xir_mixed_deep_faults.h"

static unsigned deep_source_resource_number(const char *text) {
    char *end = NULL; unsigned long value = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && value < 20000); return (unsigned)value;
}
static void deep_source_runtime_compiler_stable(const XrXirProgram *program, size_t blocks, size_t bytes) {
    XrCompileResourceStats stats = {0};
    CHECK(xr_compile_resources_stats(program->context.resources, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.allocation_count == deep_source_compiled.allocation_count && stats.allocated_bytes == deep_source_compiled.allocated_bytes);
    CHECK(stats.live_bytes == deep_source_compiled.live_bytes && stats.peak_bytes == deep_source_compiled.peak_bytes && stats.work == deep_source_compiled.work);
    CHECK(effects_compile_live == blocks && effects_compile_bytes == bytes && !effects_source_owner_count);
    CHECK(!worker_code_releases && deep_code && worker_control_code);
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4 || argc == 7); (void)deep_source_lifetime_main;
    unsigned backend = 0; while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend;
    CHECK(backend < 4); XrXirProgram *program = deep_source_lifetime_program(backend);
    const size_t blocks = effects_compile_live, bytes = effects_compile_bytes;
    deep_source_runtime_compiler_stable(program, blocks, bytes);
    if (argc != 7) {
        CHECK(!strcmp(argv[2], "axes"));
        unsigned first = argc == 4 ? deep_source_resource_number(argv[3]) : 0;
        unsigned end = argc == 4 ? first+1 : 6; CHECK(first < 6);
        for (unsigned mode = first; mode < end; ++mode) {
            (void)deep_resource_axes(program, backend, mode); deep_source_runtime_compiler_stable(program, blocks, bytes);
        }
    } else {
        CHECK(!strcmp(argv[2], "fi")); unsigned mode = deep_source_resource_number(argv[3]);
        size_t begin = deep_source_resource_number(argv[4]), end = deep_source_resource_number(argv[5]), expected = deep_source_resource_number(argv[6]);
        CHECK(mode < 6 && begin < end && end <= expected);
        CHECK(deep_resource_axes(program, backend, mode) == expected);
        for (size_t site = begin; site < end; ++site) {
            runtime_attempts = 0; runtime_fail_at = site;
            CHECK(!deep_resource_operation(program, mode, NULL, NULL) && runtime_attempts > site);
            deep_source_runtime_compiler_stable(program, blocks, bytes);
        }
        runtime_fail_at = SIZE_MAX;
        printf("Deep Source runtime complete FI backend=%s mode=%u range=%zu:%zu of%zu, everyActualOrdinal typedOOM originalDepth compilerChargesStable physicalBaseline\n",
            worker_backends[backend], mode, begin, end, expected);
    }
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !worker_control_code && !deep_code);
    CHECK(!runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    printf("Deep Source runtime backend=%s finalCodeOwnerDead bothPhysical0/0\n", worker_backends[backend]);
    return 0;
}
