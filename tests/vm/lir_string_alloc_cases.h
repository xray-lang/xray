/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_string_alloc_cases.h - String reference metadata owns its compilation lifetime
 */
#include "../lir/lir_string_fixture.h"
static void string_reference_compiler_failures(void) {
    XrLirModule *module = NULL;
    OK(string_ref_fixture(&module));
    XrLirFunction root = {0};
    OK(xr_lir_function_at(module,0,&root));
    XrRbcProgram *program = NULL;
    attempts=0;fail_at=SIZE_MAX;
    CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_OK);
    size_t total=attempts;
    xr_rbc_program_destroy(program);program=NULL;
    CHECK(total && !live_allocations);
    for (size_t point=1;point<=total;++point) {
        attempts=0;fail_at=point;
        CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_OUT_OF_MEMORY);
        CHECK(!program && !live_allocations);
    }
    fail_at=SIZE_MAX;
    CHECK(xr_rbc_compile(module,root,&program,NULL) == XR_RBC_COMPILE_OK);
    xr_lir_module_destroy(module);
    uint64_t result=0;
    CHECK(execute(program,NULL,0,&result) == XR_RBC_OK && result == 2);
    xr_rbc_program_destroy(program);
    CHECK(!live_allocations);
    printf("String reference compiler allocation failure points: %zu\n",total);
}
