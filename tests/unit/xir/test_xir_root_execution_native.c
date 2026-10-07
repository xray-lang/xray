/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_execution_native.c - Runtime-only native root qualification
 *
 * KEY CONCEPT:
 *   Separate instances share code while retaining execution and value owners.
 */
#include "xir/xxir_program.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_source_fixture_owner.h"
#include "xir_runtime_allocations.h"
#include "root_execution_native_generated.h"
#include "xir_root_execution_native_oracles.h"

int main(int argc, char **argv) {
    CHECK(argc == 2);
    instance_compile_zero();
    CHECK(!runtime_live && !runtime_bytes);
    SourceFixtureOwner owner = {0};
    source_fixture_owner_new(&owner);
    const char *outcome = NULL;
    if (!strcmp(argv[1], "root_identity")) {
        rn_identity(&owner.context);
        outcome = "native root_identity two-instance/gate/unbound-call physical0 PASS";
    } else if (!strcmp(argv[1], "resume")) {
        rn_resume_case(&owner.context);
        outcome = "native resume same-root/stale-token/owned-result physical0 PASS";
    } else if (!strcmp(argv[1], "closing")) {
        rn_closing_case(&owner.context);
        outcome = "native closing root-cleanup-once/admission/twin physical0 PASS";
    } else if (!strcmp(argv[1], "init_failure")) {
        rn_failure_case(&owner.context);
        outcome = "native init_failure partial-publication/sticky24/owned-panic physical0 PASS";
    } else CHECK(false);
    CHECK(outcome && !runtime_live && !runtime_bytes);
    source_fixture_owner_free(&owner);
    instance_compile_report();
    puts(outcome);
    return 0;
}
