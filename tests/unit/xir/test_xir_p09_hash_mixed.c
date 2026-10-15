/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_p09_hash_mixed.c - Both real VM/native directions with owned containers
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_fixture_owner.h"
#include "p09_hash_pipeline.h"
#include "p09_hash_program.h"
extern const XrXirProgramSpec p09_hash_program;
typedef struct HashMixedOwner {
    XrXirArtifact *artifact; XrXirVmBinding *bindings; XrXirCallEntry *entries;
    unsigned releases;
} HashMixedOwner;
static void hash_mixed_release(void *context) {
    HashMixedOwner *owner = context; ++owner->releases;
    xr_compile_resources_free(owner->entries); xr_compile_resources_free(owner->bindings);
    xr_xir_compile_artifact_free(owner->artifact);
    owner->entries = NULL; owner->bindings = NULL; owner->artifact = NULL;
}
int main(void) {
    SourceFixtureOwner fixture = {0}; source_fixture_owner_new(&fixture);
    for (unsigned direction = 0; direction < 2; ++direction) {
        HashMixedOwner owner = {.artifact = hash_pipeline_lower(&fixture.context)};
        CHECK(xr_xir_compile_vm_bind_table(owner.artifact, &owner.bindings, &owner.entries) == XR_XIR_OK);
        CHECK(p09_hash_program.entry_count == 6);
        /* The two callees and relays retain their real backend state and code
         * owner. The common Call boundary selects each concrete table entry. */
        owner.entries[direction ? 3 : 2] = p09_hash_program.entries[direction ? 3 : 2];
        owner.entries[direction ? 5 : 4] = p09_hash_program.entries[direction ? 5 : 4];
        XrXirProgramSpec spec = p09_hash_program; spec.entries = owner.entries;
        spec.code = (XrXirCodeLease){&owner, hash_mixed_release};
        XrXirProgram *program = NULL;
        CHECK(xr_xir_compile_program_seal(&fixture.context, &spec, &program) == XR_XIR_OK);
        hash_program_run(program);
        CHECK(owner.releases == 1 && !owner.artifact && !owner.bindings && !owner.entries);
    }
    source_fixture_owner_free(&fixture);
    puts("Map and Set real VM/native and native/VM calls, late results and COW PASS"); return 0;
}
