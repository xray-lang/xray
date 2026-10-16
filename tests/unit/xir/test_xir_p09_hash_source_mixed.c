/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_source_mixed.c - Both backend directions from one Source artifact
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_source.h"
#include "p09_hash_source_program.h"
extern const XrXirProgramSpec p09_hash_source_program;
extern const uint32_t p09_hash_source_function_ids[4];
typedef struct HashSourceMixedOwner {
    XrXirArtifact *artifact; XrXirVmBinding *bindings; XrXirCallEntry *entries;
    unsigned releases;
} HashSourceMixedOwner;
static void hash_source_mixed_release(void *context) {
    HashSourceMixedOwner *owner = context; ++owner->releases;
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner->bindings);
    xr_xir_compile_artifact_free(owner->artifact);
    owner->entries = NULL; owner->bindings = NULL; owner->artifact = NULL;
}
int main(void) {
    SourceFixtureOwner fixture = {0}; source_fixture_owner_new(&fixture);
    for (unsigned direction = 0; direction < 2; ++direction) {
        HashSourceMixedOwner owner = {.artifact = hash_source_lower(&fixture.context)};
        CHECK(xr_xir_compile_vm_bind_table(owner.artifact, &owner.bindings, &owner.entries) == XR_XIR_OK);
        uint32_t entries[4]; hash_source_entries(xr_xir_compile_artifact_module(owner.artifact), entries);
        CHECK(!memcmp(entries, p09_hash_source_function_ids, sizeof(entries)));
        uint32_t map = entries[direction ? 1 : 0], set = entries[direction ? 3 : 2];
        owner.entries[map] = p09_hash_source_program.entries[map];
        owner.entries[set] = p09_hash_source_program.entries[set];
        XrXirProgramSpec spec = p09_hash_source_program; spec.entries = owner.entries;
        spec.code = (XrXirCodeLease){&owner, hash_source_mixed_release};
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&fixture.context, &spec, &program) == XR_XIR_OK);
        hash_source_program_run(program, entries[1], entries[3]);
        CHECK(owner.releases == 1 && !owner.artifact && !owner.bindings && !owner.entries);
    }
    source_fixture_owner_free(&fixture);
    puts("Source Map/Set VM/native and native/VM calls, late results and COW PASS"); return 0;
}
