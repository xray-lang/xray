/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_identity_vm_resources.c - Real Instance preparation fault gates
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "root resources %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_root_identity_fixture.h"
#include "xir_root_identity_observer.h"
#include "xir_root_identity_cases.h"
#include "xir_root_identity_vm_resources_prepare.h"

int main(int argc, char **argv) {
    CHECK(argc == 2); setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(runtime_fail_at == SIZE_MAX && effects_compile_fail_at == SIZE_MAX);
    if (!strcmp(argv[1], "normal")) rr_normal();
    else if (!strcmp(argv[1], "create_oom")) rr_create_oom();
    else if (!strcmp(argv[1], "prepare_oom")) rr_prepare_oom();
    else if (!strcmp(argv[1], "prepare_budget")) rr_prepare_budget();
    else CHECK(false);
    CHECK(!ri_observed && !runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    CHECK(!runtime_owned && !runtime_owned_capacity && !effects_source_owner_count);
    puts("root identity resource frontier: physical0, full_P3/Task322/Goal OPEN");
    return 0;
}
