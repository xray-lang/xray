/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_index_resources.c - Actual allocation and cancellation boundaries
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "p09_hash_source_check.h"
#include "p09_map_index_input.h"

#include "p09_map_index_resource_cases.h"
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && (!strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"))));
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirSourceResult result = {0}; CHECK(hash_source_check(&owner.context,index_source,&result) == XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
    XrXirArtifact *lowered = hash_source_replay(&owner.context,&result);
    uint32_t entries[INDEX_COUNT]; index_entries(xr_xir_compile_artifact_module(lowered),entries);
    XrXirProgram *program = NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program) == XR_XIR_OK && !lowered);
    index_allocations(program,entries,argc == 1);
    if (argc == 1 || !strcmp(argv[1],"--cancel")) index_cancellation(program,entries);
    xr_xir_compile_program_drop(program); source_fixture_owner_free(&owner);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Source Map index actual runtime allocation ownership PASS"); return 0;
}
