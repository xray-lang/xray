/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_set_algebra_resources_native.c - Actual generated C allocation boundaries
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "p09_set_algebra_resource_cases.h"
extern const XrXirProgramSpec p09_set_algebra_program;
extern const uint32_t p09_set_algebra_ids[5];
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && (!strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"))));
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,&p09_set_algebra_program,&program) == XR_XIR_OK);
    algebra_allocations(program,p09_set_algebra_ids,argc == 1);
    if (argc == 1 || !strcmp(argv[1],"--cancel")) algebra_cancellation(program,p09_set_algebra_ids);
    xr_xir_compile_program_drop(program); source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Generated C Set algebra actual runtime allocation ownership PASS"); return 0;
}
