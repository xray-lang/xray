/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_identity_vm.c - Authenticated VM execution and host refusals
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "root identity %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_root_identity_fixture.h"
#include "xir_root_identity_observer.h"
#include "xir_root_identity_cases.h"

int main(int argc, char **argv) {
    CHECK(argc == 2); setvbuf(stdout, NULL, _IONBF, 0);
    if (!strcmp(argv[1], "identity")) ri_identity_case();
    else if (!strcmp(argv[1], "resume")) ri_resume_case();
    else if (!strcmp(argv[1], "closing")) ri_closing_case();
    else if (!strcmp(argv[1], "initialization_failure")) ri_initialization_failure_case();
    else CHECK(false);
    CHECK(!ri_observed && !runtime_live && !runtime_bytes);
    effects_source_owners_free();
    return 0;
}
