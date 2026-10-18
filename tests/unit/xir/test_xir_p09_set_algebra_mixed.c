/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_set_algebra_mixed.c - Real generic Map calls in both backend directions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_set_algebra_source.h"
#include "p09_set_algebra_program.h"
extern const XrXirProgramSpec p09_set_algebra_program;
extern const uint32_t p09_set_algebra_ids[5];
#include "p09_set_algebra_mixed_code.h"
int main(void) {
    SourceFixtureOwner fixture = {0}; source_fixture_owner_new(&fixture);
    XrXirSourceResult result = {0}; CHECK(hash_source_check(&fixture.context, algebra_source, &result) == XR_XIR_OK);
    algebra_query(xr_xir_compile_source_snapshot_view(result.snapshot));
    AlgebraMixedCode code = {hash_source_replay(&fixture.context, &result), 1};
    const XrXirModule *module = xr_xir_compile_artifact_module(code.artifact);
    uint32_t entries[5]; algebra_entries(module, entries);
    CHECK(!memcmp(entries, p09_set_algebra_ids, sizeof(entries)));
    uint32_t callee = algebra_generic_callee(module, entries);
    for (uint32_t direction = 0; direction < 2; ++direction) {
        AlgebraMixedLease lease = {0};
        XrXirProgram *program = algebra_mixed_program(&fixture.context, &code, &lease,
            &p09_set_algebra_program, direction ? entries[3] : callee);
        algebra_program_run(program, entries);
        CHECK(lease.releases == 1 && !lease.code && !lease.bindings && !lease.entries && code.owners == 1);
    }
    algebra_code_drop(&code); CHECK(!code.artifact && !code.owners);
    source_fixture_owner_free(&fixture);
    puts("Set algebra both real mixed-call directions, exact code leases and owned results PASS"); return 0;
}
