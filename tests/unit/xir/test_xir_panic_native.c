/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_panic_native.c - Runtime-only generated panic execution
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_runtime_allocations.h"
#include "xir_panic_cases.h"
XR_DATA const XrXirProgramSpec panic_source_program;
XR_DATA const XrXirProgramSpec panic_bad_program;
XR_DATA const uint32_t panic_source_functions[PANIC_FUNCTIONS];
int main(void) {
    XrXirProgram *program = NULL;
    CHECK(xr_xir_program_seal(&panic_source_program,
        (XrXirProgramBudget) {16777216, 64000000}, &program) == XR_XIR_OK);
    panic_cases(program, panic_source_functions);
    CHECK(xr_xir_program_seal(&panic_bad_program,
        (XrXirProgramBudget) {16777216, 64000000}, &program) == XR_XIR_OK);
    panic_sticky(program, panic_bad_program.declarations->entry_function);
    puts("Native panic execution matched independent values, channels and physical ownership");
    return 0;
}
