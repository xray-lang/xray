/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_map_alloc_cases.h - Map compiler and runtime allocation failure accounting
 *
 * KEY CONCEPT:
 *   Every observed failure leaves the receiving owner unpublished or drains
 *   its already owned outcome before the next attempt.
 */
#include "../lir/lir_map_fixture.h"
static void map_compiler_failures(void) {
    XrLirModule *module = map_fixture(1);
    XrLirFunction root = {module, 1};
    XrRbcProgram *program = NULL;
    attempts = 0; fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    size_t total = attempts;
    xr_rbc_program_destroy(program); program = NULL;
    CHECK(total && !live_allocations);
    for (size_t n = 1; n <= total; ++n) {
        attempts = 0; fail_at = n;
        CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_rbc_compile(module, root, &program, NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    size_t retained = live_allocations;
    uint64_t bits = 0;
    attempts = 0;
    CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OK && bits == 42);
    size_t runtime = attempts;
    CHECK(live_allocations == retained);
    for (size_t n = 1; n <= runtime; ++n) {
        attempts = 0; fail_at = n;
        CHECK(execute(program, NULL, 0, &bits) == XR_RBC_OUT_OF_MEMORY);
        CHECK(live_allocations == retained);
    }
    fail_at = SIZE_MAX;
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("Map compiler/runtime allocation failures %zu/%zu drained\n", total, runtime);
    attempts = 0;
}
