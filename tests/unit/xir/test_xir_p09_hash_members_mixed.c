/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_members_mixed.c - Real generic Map calls in both backend directions
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_members_source.h"
#include "p09_hash_members_program.h"
extern const XrXirProgramSpec p09_hash_members_program;
extern const uint32_t p09_hash_members_ids[5];
typedef struct MembersMixedOwner {
    XrXirArtifact *artifact;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
    uint32_t releases;
} MembersMixedOwner;
static void members_mixed_release(void *opaque) {
    MembersMixedOwner *owner = opaque; ++owner->releases;
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner->bindings);
    xr_xir_compile_artifact_free(owner->artifact);
    owner->entries = NULL; owner->bindings = NULL; owner->artifact = NULL;
}
int main(void) {
    SourceFixtureOwner fixture = {0}; source_fixture_owner_new(&fixture);
    for (uint32_t direction = 0; direction < 2; ++direction) {
        XrXirSourceResult result = {0}; CHECK(hash_source_check(&fixture.context, members_source, &result) == XR_XIR_OK);
        members_query(xr_xir_compile_source_snapshot_view(result.snapshot));
        MembersMixedOwner owner = {.artifact = hash_source_replay(&fixture.context, &result)};
        const XrXirModule *module = xr_xir_compile_artifact_module(owner.artifact);
        uint32_t entries[5]; members_entries(module, entries);
        CHECK(!memcmp(entries, p09_hash_members_ids, sizeof(entries)));
        uint32_t callee = UINT32_MAX;
        const XrXirFunction *root = &module->functions[entries[3]];
        for (uint32_t i = 0; i < root->instruction_count; ++i)
            if (root->instructions[i].op == XR_XIR_CALL) {
                CHECK(callee == UINT32_MAX); callee = (uint32_t)root->instructions[i].immediate;
            }
        CHECK(callee < module->function_count && callee != entries[3]);
        CHECK(xr_xir_compile_vm_bind_table(owner.artifact, &owner.bindings, &owner.entries) == XR_XIR_OK);
        uint32_t native = direction ? entries[3] : callee;
        owner.entries[native] = p09_hash_members_program.entries[native];
        XrXirProgramSpec spec = p09_hash_members_program; spec.entries = owner.entries;
        spec.code = (XrXirCodeLease){&owner, members_mixed_release};
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&fixture.context, &spec, &program) == XR_XIR_OK);
        members_program_run(program, entries);
        CHECK(owner.releases == 1 && !owner.artifact && !owner.bindings && !owner.entries);
    }
    source_fixture_owner_free(&fixture);
    puts("Source generic Map calls VM/native and native/VM with late results PASS"); return 0;
}
