/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_float_native.c - Runtime-only native Float source expectations
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_source_float_cases.h"
XR_DATA const XrXirProgramSpec float_source_program;
XR_DATA const uint32_t float_source_functions[FLOAT_FUNCTION_COUNT];
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&float_source_program, (XrXirProgramBudget) {2097152, 16000000}, &program) == XR_XIR_OK);
    source_float_program_cases(program, float_source_functions);
    puts("Native Float source matched independent value, order, ownership and physical-release expectations");
    return 0;
}
