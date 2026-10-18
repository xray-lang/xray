/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_key_path_mixed.c - Ordinary ref calls retain key owners across both code providers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"
#include "p09_map_key_path_input.h"
#include "p09_map_key_path_program.h"
#include "p09_hash_foreach_mixed_code.h"
extern const XrXirProgramSpec *const p09_map_key_path_programs[8];
extern const uint32_t p09_map_key_path_ids[8];
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);XrXirSourceResult result={0};
    CHECK(hash_source_check(&owner.context,map_key_sources[2].source,&result)==XR_XIR_OK);
    ForeachMixedCode code={hash_source_replay(&owner.context,&result),1};
    const XrXirModule *module=xr_xir_compile_artifact_module(code.artifact);uint32_t helper=UINT32_MAX;
    uint32_t entry=key_source_entry(module);CHECK(entry==p09_map_key_path_ids[2]);
    for(uint32_t f=0;f<module->function_count;++f)
        if(module->functions[f].name_length==4 && !memcmp(module->functions[f].name,"edit",4)) {
            CHECK(helper==UINT32_MAX);helper=f;
        }
    CHECK(helper<module->function_count && helper!=entry);
    for(unsigned direction=0;direction<2;++direction) {
        ForeachMixedLease lease={0};
        XrXirProgram *program=foreach_mixed_program(&owner.context,&code,&lease,p09_map_key_path_programs[2],direction?entry:helper);
        key_source_run(program,entry,map_key_sources[2].expected);
        CHECK(lease.releases==1 && !lease.code && !lease.entries && !lease.bindings && code.owners==1);
    }
    foreach_code_drop(&code);CHECK(!code.artifact && !code.owners);source_fixture_owner_free(&owner);
    puts("Both actual mixed ref calls retain string selectors across suspension and release their code leases PASS");return 0;
}
