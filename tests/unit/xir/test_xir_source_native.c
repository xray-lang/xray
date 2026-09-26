/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_native.c - Real source closure in a runtime-only executable
 *
 * KEY CONCEPT:
 *   Native execution uses fixed expectations without the compiler or VM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_cases.h"
XR_DATA const XrXirProgramSpec fixture_source_program;
XR_DATA const uint32_t fixture_source_result, fixture_source_advance;
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&fixture_source_program, 262144, &program) == XR_XIR_OK);
    XrXirValue results[2] = {{0}, {0}};
    source_pair(program, fixture_source_program.declarations->entry_function,
        fixture_source_result, fixture_source_advance, results);
    xr_xir_program_drop(program);
    source_result_drop(&results[0]); source_result_drop(&results[1]);
    puts("Real source modules matched independent native expectations");
    return 0;
}
