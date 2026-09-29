/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_native.c - Source cleanup in generated native code without a VM
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_cleanup_source_cases.h"
XR_DATA const XrXirProgramSpec cleanup_source_program;
XR_DATA const uint32_t cleanup_source_functions[CLEANUP_SOURCE_FUNCTIONS];
XR_DATA const uint32_t cleanup_source_fatal_functions[4];
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 3 && !strcmp(argv[1], "--fatal")));
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&cleanup_source_program, (XrXirProgramBudget){16777216, 64000000}, &program) == XR_XIR_OK);
    if (argc == 3) {
        unsigned mode = (unsigned)atoi(argv[2]); CHECK(mode < 4);
        cleanup_source_fatal(program, cleanup_source_fatal_functions[mode]);
    }
    cleanup_source_allocations(program, cleanup_source_functions);
    cleanup_source_cases(program, cleanup_source_functions);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Source cleanup native: independent output, pending results and physical release passed");
    return 0;
}
