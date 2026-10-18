/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_worker_mixed.c - Actual worker arguments and retained terminal container results
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_worker_program.h"
#include "p09_hash_source_check.h"
#include "p09_hash_worker_input.h"
#include "p09_hash_foreach_mixed_code.h"
extern const XrXirProgramSpec p09_hash_worker_program;
extern const uint32_t p09_hash_worker_ids[3];
extern const uint32_t p09_hash_worker_callee;
int main(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirSourceResult result={0};CHECK(hash_source_check(&owner.context,worker_source,&result)==XR_XIR_OK);
    ForeachMixedCode code={hash_source_replay(&owner.context,&result),1};uint32_t entries[3],worker=0;
    worker_entries(xr_xir_compile_artifact_module(code.artifact),entries,&worker);
    CHECK(!memcmp(entries,p09_hash_worker_ids,sizeof(entries)) && worker==p09_hash_worker_callee);
    for(unsigned direction=0;direction<2;++direction) {
        ForeachMixedLease lease={0};
        XrXirProgram *program=foreach_mixed_program(&owner.context,&code,&lease,&p09_hash_worker_program,direction?entries[0]:worker);
        worker_program_run(program,entries);
        CHECK(lease.releases==1 && !lease.code && !lease.entries && !lease.bindings && code.owners==1);
    }
    foreach_code_drop(&code);CHECK(!code.artifact && !code.owners);source_fixture_owner_free(&owner);
    puts("Both real mixed hash worker directions and escaped Task container ownership PASS");return 0;
}
