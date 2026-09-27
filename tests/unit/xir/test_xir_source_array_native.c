/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_array_native.c - Runtime-only native Array source expectations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_source_array_cases.h"
XR_DATA const XrXirProgramSpec array_source_program;
XR_DATA const XrXirProgramSpec array_bounds_program;
XR_DATA const uint32_t array_source_functions[ARRAY_FUNCTION_COUNT];
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&array_source_program, 262144, &program) == XR_XIR_OK);
    source_array_program_cases(program, array_source_functions);
    CHECK(xr_xir_program_seal(&array_bounds_program, 262144, &program) == XR_XIR_OK);
    source_array_sticky_bounds(program, array_bounds_program.declarations->entry_function);
    puts("Native Array source matched independent value, order, ownership and physical-release expectations");
    return 0;
}
