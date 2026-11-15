/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_string_alloc_cases.h - Qualified String references require a current execution binding
 */
#include "../lir/lir_string_fixture.h"
static void string_reference_rejection(void) {
    XrLirModule *module=NULL;OK(string_ref_fixture(&module));
    XrLirFunction root={0};OK(xr_lir_function_at(module,0,&root));
    XrRbcProgram *program=NULL;
    CHECK(xr_rbc_compile(module,root,&program,NULL)==XR_RBC_COMPILE_UNSUPPORTED);
    CHECK(!program && !live_allocations);xr_lir_module_destroy(module);
}
