/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_foreach_resources.c - Actual allocation and cancellation boundaries
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "p09_hash_foreach_source.h"

#include "p09_hash_foreach_resource_cases.h"
#include "p09_hash_foreach_matrix_input.h"
extern const XrXirProgramSpec *const p09_foreach_matrix_programs[4];
extern const uint32_t p09_foreach_matrix_ids[4];
int main(int argc, char **argv) {
    CHECK(argc>=1 && argc<=3);
    bool full=argc==1 || !strcmp(argv[1],"--full");
    CHECK(full || !strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"));
    bool matrix=argc==3;uint32_t row=matrix?(uint32_t)strtoul(argv[2],NULL,10):0;
    CHECK(!matrix || row<4);
    if(matrix){foreach_resource_count=1;foreach_resource_expected[0]=foreach_matrix_yields[row];}
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirSourceResult result = {0}; CHECK(hash_source_check(&owner.context,(matrix?foreach_matrix_sources[row]:foreach_source),&result) == XR_XIR_OK);
    foreach_query(xr_xir_compile_source_snapshot_view(result.snapshot));
    XrXirArtifact *lowered = hash_source_replay(&owner.context,&result);
    uint32_t entries[5]={0};
    if(matrix)entries[0]=foreach_matrix_entry(xr_xir_compile_artifact_module(lowered));
    else foreach_entries(xr_xir_compile_artifact_module(lowered),entries);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program) == XR_XIR_OK && !lowered);
    foreach_allocations(program,entries,full);
    if (full || !strcmp(argv[1],"--cancel")) foreach_cancellation(program,entries);
    xr_xir_compile_program_drop(program); source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Source Map/Set forEach actual runtime allocation ownership PASS"); return 0;
}
