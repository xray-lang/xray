/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_program_native.c - Generated native programs with no VM linkage
 *
 * KEY CONCEPT:
 *   Runtime-only execution checks fixed expectations without a VM oracle.
 */
#include "xir/xxir_program.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_program_cases.h"
#include "xir_capture_cases.h"
extern const XrXirProgramSpec captures_program;
extern const XrXirProgramSpec program0_program, program1_program, program2_program;
int main(void) {
    const XrXirProgramSpec *specs[] = {&program0_program, &program1_program, &program2_program};
    for (uint32_t mode = 0; mode < 3; ++mode) {
        XrXirProgram *program = NULL;
        CHECK(xr_xir_program_seal(specs[mode], 65536, &program) == XR_XIR_OK);
        program_cases(program, mode);
    }
    XrXirProgram *captures = NULL;
    CHECK(xr_xir_program_seal(&captures_program,65536,&captures) == XR_XIR_OK);
    capture_cases(captures);
    puts("Native capture environments, two suspensions, cancellation and escaped ownership passed");
    puts("Native module programs match independent output, state and lifetime expectations");
    return 0;
}
