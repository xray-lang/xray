/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_index_resources_mixed.c - Physical faults across both real call directions
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
extern const XrXirProgramSpec p09_map_index_program;
extern const uint32_t p09_map_index_ids[INDEX_COUNT];
typedef struct MembersResourceCode {
    XrXirArtifact *artifact;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
    uint32_t releases;
} MembersResourceCode;
static void members_resource_release(void *opaque) {
    MembersResourceCode *owner = opaque; ++owner->releases;
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner->bindings);
    xr_xir_compile_artifact_free(owner->artifact);
    owner->entries = NULL; owner->bindings = NULL; owner->artifact = NULL;
}
int main(int argc, char **argv) {
    CHECK(argc == 1 || (argc == 2 && (!strcmp(argv[1],"--normal") || !strcmp(argv[1],"--cancel"))));
    SourceFixtureOwner fixture = {0}; source_fixture_owner_new(&fixture);
    for (uint32_t direction = 0; direction < 2; ++direction) {
        XrXirSourceResult result = {0}; CHECK(hash_source_check(&fixture.context,index_source,&result) == XR_XIR_OK);
        CHECK(xr_xir_compile_source_snapshot_view(result.snapshot)->complete);
        MembersResourceCode owner = {.artifact=hash_source_replay(&fixture.context,&result)};
        const XrXirModule *module = xr_xir_compile_artifact_module(owner.artifact);
        uint32_t entries[INDEX_COUNT]; index_entries(module,entries);
        CHECK(!memcmp(entries,p09_map_index_ids,sizeof(entries)));
        uint32_t callee = UINT32_MAX;
        const XrXirFunction *root = &module->functions[entries[INDEX_GENERIC]];
        for (uint32_t i = 0; i < root->instruction_count; ++i)
            if (root->instructions[i].op == XR_XIR_CALL) {
                CHECK(callee == UINT32_MAX); callee = (uint32_t)root->instructions[i].immediate;
            }
        CHECK(callee < module->function_count && callee != entries[INDEX_GENERIC]);
        CHECK(xr_xir_compile_vm_bind_table(owner.artifact,&owner.bindings,&owner.entries) == XR_XIR_OK);
        if (!direction) owner.entries[callee]=p09_map_index_program.entries[callee];
        else {
            owner.entries[entries[INDEX_GENERIC]]=p09_map_index_program.entries[entries[INDEX_GENERIC]];
            owner.entries[entries[INDEX_CAUGHT]]=p09_map_index_program.entries[entries[INDEX_CAUGHT]];
        }
        XrXirProgramSpec spec = p09_map_index_program; spec.entries = owner.entries;
        spec.code = (XrXirCodeLease){&owner,members_resource_release};
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&fixture.context,&spec,&program) == XR_XIR_OK);
        index_allocations(program,entries,argc == 1);
        if (argc == 1 || !strcmp(argv[1],"--cancel")) index_cancellation(program,entries);
        xr_xir_compile_program_drop(program);
        CHECK(owner.releases == 1 && !owner.artifact && !owner.bindings && !owner.entries);
        CHECK(!runtime_live && !runtime_bytes);
    }
    source_fixture_owner_free(&fixture);
    puts("Both real Map index mixed-call directions physical ownership PASS"); return 0;
}
