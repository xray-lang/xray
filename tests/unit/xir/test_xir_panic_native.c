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
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir_panic_cases.h"
XR_DATA const XrXirProgramSpec panic_source_program;
XR_DATA const XrXirProgramSpec panic_bad_program;
XR_DATA const uint32_t panic_source_functions[PANIC_FUNCTIONS];
int main(void) {
    const XrXirCompileContext context = *effects_source_owner(UINT64_C(16)*1024*1024, UINT64_C(64000000));
    const XrXirCompileContext bad_context = *effects_source_owner(UINT64_C(16)*1024*1024, UINT64_C(64000000));
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&context, &panic_source_program, &program) == XR_XIR_OK);
    panic_cases(program, panic_source_functions); program = NULL;
    CHECK(xr_xir_compile_program_seal(&bad_context, &panic_bad_program, &program) == XR_XIR_OK);
    panic_sticky(program, panic_bad_program.declarations->entry_function);
    effects_source_owners_free();
    puts("Native panic execution matched independent values, channels and physical ownership");
    return 0;
}
