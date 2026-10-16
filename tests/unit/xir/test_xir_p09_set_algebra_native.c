/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_set_algebra_native.c - Real generated portable hash instructions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_set_algebra_program.h"
extern const XrXirProgramSpec p09_set_algebra_program;
extern const uint32_t p09_set_algebra_ids[5];
int main(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context, &p09_set_algebra_program, &program) == XR_XIR_OK);
    algebra_program_run(program, p09_set_algebra_ids);
    source_fixture_owner_free(&owner);
    puts("Source ordered hash projections in generated portable C11 PASS"); return 0;
}
