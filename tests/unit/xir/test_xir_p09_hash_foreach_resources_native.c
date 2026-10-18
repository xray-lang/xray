/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_foreach_resources_native.c - Actual generated C allocation boundaries
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "p09_hash_foreach_resource_cases.h"
#include "p09_hash_foreach_matrix_input.h"
extern const XrXirProgramSpec *const p09_foreach_matrix_programs[4];
extern const uint32_t p09_foreach_matrix_ids[4];
extern const XrXirProgramSpec p09_hash_foreach_program;
extern const uint32_t p09_hash_foreach_ids[5];
int main(int argc, char **argv) {
    CHECK(argc>=1 && argc<=3);
    bool full=argc==1 || !strcmp(argv[1],"--full");
    CHECK(full || !strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"));
    bool matrix=argc==3;uint32_t row=matrix?(uint32_t)strtoul(argv[2],NULL,10):0;
    CHECK(!matrix || row<4);
    if(matrix){foreach_resource_count=1;foreach_resource_expected[0]=foreach_matrix_yields[row];}
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_program_seal(&owner.context,(matrix?p09_foreach_matrix_programs[row]:&p09_hash_foreach_program),&program) == XR_XIR_OK);
    const uint32_t *entries=matrix?p09_foreach_matrix_ids+row:p09_hash_foreach_ids;
    foreach_allocations(program,entries,full);
    if (full || !strcmp(argv[1],"--cancel")) foreach_cancellation(program,entries);
    xr_xir_compile_program_drop(program); source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Generated C Map/Set forEach actual runtime allocation ownership PASS"); return 0;
}
