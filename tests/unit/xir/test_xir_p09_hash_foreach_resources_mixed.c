/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_foreach_resources_mixed.c - Physical faults across both real call directions
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
extern const XrXirProgramSpec p09_hash_foreach_program;
extern const uint32_t p09_hash_foreach_ids[5];
#include "p09_hash_foreach_mixed_code.h"
int main(int argc, char **argv) {
    CHECK(argc>=1 && argc<=3);
    bool full=argc==1 || !strcmp(argv[1],"--full");
    CHECK(full || !strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"));
    bool matrix=argc==3;uint32_t row=matrix?(uint32_t)strtoul(argv[2],NULL,10):0;
    CHECK(!matrix || row<4);
    if(matrix){foreach_resource_count=1;foreach_resource_expected[0]=foreach_matrix_yields[row];}
    SourceFixtureOwner fixture = {0}; source_fixture_owner_new(&fixture);
    XrXirSourceResult result = {0}; CHECK(hash_source_check(&fixture.context, (matrix?foreach_matrix_sources[row]:foreach_source), &result) == XR_XIR_OK);
    foreach_query(xr_xir_compile_source_snapshot_view(result.snapshot));
    ForeachMixedCode code = {hash_source_replay(&fixture.context, &result), 1};
    const XrXirModule *module = xr_xir_compile_artifact_module(code.artifact);
    uint32_t entries[5]={0};const XrXirProgramSpec *native=&p09_hash_foreach_program;
    if(matrix) {
        uint32_t entry=foreach_matrix_entry(module);CHECK(entry==p09_foreach_matrix_ids[row]);
        for(unsigned i=0;i<5;++i)entries[i]=entry;native=p09_foreach_matrix_programs[row];
    } else {foreach_entries(module,entries);CHECK(!memcmp(entries,p09_hash_foreach_ids,sizeof(entries)));}
    uint32_t callee=foreach_generic_callee(module,entries);
    for (uint32_t direction = 0; direction < 2; ++direction) {
        ForeachMixedLease lease = {0};
        XrXirProgram *program = foreach_mixed_program(&fixture.context, &code, &lease,
            native, direction ? entries[3] : callee);
        foreach_allocations(program, entries, full);
        if (full || !strcmp(argv[1], "--cancel")) foreach_cancellation(program, entries);
        xr_xir_compile_program_drop(program);
        CHECK(!runtime_live && !runtime_bytes);
        CHECK(lease.releases == 1 && !lease.code && !lease.bindings && !lease.entries && code.owners == 1);
    }
    foreach_code_drop(&code); CHECK(!code.artifact && !code.owners);
    source_fixture_owner_free(&fixture);
    puts("Map/Set forEach both real mixed-call directions, exact code leases and owned results PASS"); return 0;
}
