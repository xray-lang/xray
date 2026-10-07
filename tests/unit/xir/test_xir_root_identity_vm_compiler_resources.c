/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_identity_vm_compiler_resources.c - Compiler ledger qualification
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_vm.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "root compiler resources %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_root_identity_fixture.h"
#include "xir_root_identity_observer.h"
#include "xir_root_identity_cases.h"
#include "xir_root_identity_vm_compiler_resources.h"

int main(int argc, char **argv) {
    CHECK(argc >= 2); setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(runtime_fail_at == SIZE_MAX && effects_compile_fail_at == SIZE_MAX);
    if (!strcmp(argv[1], "normal") || !strcmp(argv[1], "census")) {
        CHECK(argc == 2);
        if (!strcmp(argv[1], "normal")) rb_original_cases();
        for (unsigned variant = 0; variant < RB_VARIANTS; ++variant) rb_success_census(variant, false);
        puts("root compiler census: allocation denominators and budget fields require independent freeze; qualification OPEN");
    } else {
        rb_arguments(argc, argv);
        rb_qualification(argv[1]);
    }
    CHECK(!ri_observed && !runtime_live && !runtime_bytes && !effects_compile_live && !effects_compile_bytes);
    CHECK(!runtime_owned && !runtime_owned_capacity && !effects_source_owner_count);
    CHECK(effects_compile_fail_at == SIZE_MAX && runtime_fail_at == SIZE_MAX);
    puts("root compiler frontier: physical0, generic expansion/full P3/Task322/Goal OPEN");
    return 0;
}
