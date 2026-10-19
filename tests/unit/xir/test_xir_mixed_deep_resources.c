/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_deep_resources.c - Finite deep recursion and complete ordinal fault shards
 */
#define XR_DEEP_RESOURCE_MAIN
#include "test_xir_mixed_deep_recursion.c"
#include "xir_mixed_deep_faults.h"
static unsigned deep_resource_number(const char *text) {
    char *end = NULL; unsigned long value = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && value < 20000); return (unsigned)value;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4 || argc == 7); (void)deep_main; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], worker_backends[backend])) ++backend;
    CHECK(backend < 4); XrXirProgram *program = deep_program(backend);
    if (argc != 7) {
        CHECK(!strcmp(argv[2], "axes"));
        if (argc == 4) {
            unsigned mode = deep_resource_number(argv[3]); CHECK(mode < 6); (void)deep_resource_axes(program, backend, mode);
        } else for (unsigned mode = 0; mode < 6; ++mode) (void)deep_resource_axes(program, backend, mode);
    } else {
        CHECK(!strcmp(argv[2], "fi")); unsigned mode = deep_resource_number(argv[3]);
        size_t begin = deep_resource_number(argv[4]), end = deep_resource_number(argv[5]), expected = deep_resource_number(argv[6]);
        CHECK(mode < 6 && begin < end && end <= expected);
        CHECK(deep_resource_axes(program, backend, mode) == expected);
        for (size_t site = begin; site < end; ++site) {
            runtime_attempts = 0; runtime_fail_at = site;
            CHECK(!deep_resource_operation(program, mode, NULL, NULL) && runtime_attempts > site);
        }
        runtime_fail_at = SIZE_MAX;
        printf("Deep complete FI backend=%s mode=%u range=%zu:%zu of%zu, each actualordinal hit typedOOM originalDepth physical0/0\n",
            worker_backends[backend], mode, begin, end, expected);
    }
    xr_xir_compile_program_drop(program); CHECK(worker_code_releases == 1 && !worker_control_code && !deep_code);
    CHECK(!runtime_live && !runtime_bytes); effects_source_owners_free(); return 0;
}
