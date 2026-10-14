/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_library_resources.c - Finite private Library state execution
 */
#define XR_LIBRARY_COUNTER_RESOURCE_MAIN
#include "test_xir_mixed_library_counter.c"
#include "xir_mixed_library_faults.h"
int main(int argc, char **argv) {
    CHECK(argc == 3); (void)counter_normal_main;
    unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], counter_backends[backend])) ++backend;
    CHECK(backend < 4);
    XrXirProgram *program = library_counter_program(backend);
    if (!strcmp(argv[2], "axes")) counter_axes(program, backend);
    else { CHECK(!strcmp(argv[2], "fi")); counter_faults(program, backend); }
    xr_xir_compile_program_drop(program); CHECK(counter_code_releases == 1 && !runtime_live && !runtime_bytes);
    effects_source_owners_free(); return 0;
}
