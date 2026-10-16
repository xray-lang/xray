/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_map_index_mixed.c - Required lookup and caught panic cross backends
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source_check.h"
#include "p09_map_index_input.h"
#include "xir_runtime_allocations.h"
#include "p09_map_index_program.h"
extern const XrXirProgramSpec p09_map_index_program;
extern const uint32_t p09_map_index_ids[INDEX_COUNT];
typedef struct IndexMixedOwner {
    XrXirArtifact *artifact;
    XrXirVmBinding *bindings;
    XrXirCallEntry *entries;
    uint32_t releases;
} IndexMixedOwner;
static void index_mixed_release(void *opaque) {
    IndexMixedOwner *owner = opaque; ++owner->releases;
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner->bindings);
    xr_xir_compile_artifact_free(owner->artifact);
    owner->entries = NULL; owner->bindings = NULL; owner->artifact = NULL;
}
int main(void) {
    SourceFixtureOwner fixture = {0}; source_fixture_owner_new(&fixture);
    for (uint32_t direction = 0; direction < 2; ++direction) {
        XrXirSourceResult source = {0};
        CHECK(hash_source_check(&fixture.context, index_source, &source) == XR_XIR_OK);
        IndexMixedOwner owner = {.artifact = hash_source_replay(&fixture.context, &source)};
        const XrXirModule *module = xr_xir_compile_artifact_module(owner.artifact);
        uint32_t entries[INDEX_COUNT]; index_entries(module, entries);
        CHECK(!memcmp(entries, p09_map_index_ids, sizeof(entries)));
        uint32_t callee = UINT32_MAX;
        const XrXirFunction *root = &module->functions[entries[INDEX_GENERIC]];
        for (uint32_t i = 0; i < root->instruction_count; ++i)
            if (root->instructions[i].op == XR_XIR_CALL) {
                CHECK(callee == UINT32_MAX); callee = (uint32_t)root->instructions[i].immediate;
            }
        CHECK(callee < module->function_count && callee != entries[INDEX_GENERIC]);
        CHECK(xr_xir_compile_vm_bind_table(owner.artifact, &owner.bindings, &owner.entries) == XR_XIR_OK);
        if (!direction) owner.entries[callee] = p09_map_index_program.entries[callee];
        else {
            owner.entries[entries[INDEX_GENERIC]] = p09_map_index_program.entries[entries[INDEX_GENERIC]];
            owner.entries[entries[INDEX_CAUGHT]] = p09_map_index_program.entries[entries[INDEX_CAUGHT]];
        }
        XrXirProgramSpec spec = p09_map_index_program; spec.entries = owner.entries;
        spec.code = (XrXirCodeLease){&owner, index_mixed_release};
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&fixture.context, &spec, &program) == XR_XIR_OK);
        index_program_run(program, entries); CHECK(!runtime_live && !runtime_bytes);
        CHECK(owner.releases == 1 && !owner.artifact && !owner.bindings && !owner.entries);
    }
    source_fixture_owner_free(&fixture);
    puts("Required Map index and E0431 cross VM/native and native/VM PASS"); return 0;
}
