/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_diamond_resources.c - Finite diamond initialization and complete runtime faults
 */
#define XR_DIAMOND_RESOURCE_MAIN
#include "test_xir_mixed_diamond.c"
#include "xir_mixed_diamond_faults.h"
static int diamond_resources_main(int argc, char **argv) {
    CHECK(argc == 3); (void)diamond_normal_main; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], diamond_backends[backend])) ++backend;
    CHECK(backend < 4); bool fi = !strcmp(argv[2], "fi"); CHECK(fi || !strcmp(argv[2], "axes"));
    XrXirProgram *program = diamond_program(backend); diamond_fault_axes(program, backend, fi);
    xr_xir_compile_program_drop(program); CHECK(diamond_code_releases == 1 && !diamond_code && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); return 0;
}
int main(int argc, char **argv) { return diamond_resources_main(argc, argv); }
