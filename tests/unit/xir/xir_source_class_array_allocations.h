/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_class_array_allocations.h - Owned class Array producer failures
 *
 * KEY CONCEPT:
 *   Every observed source allocation is failed without publishing partial Checked.
 */
#ifndef XIR_SOURCE_CLASS_ARRAY_ALLOCATIONS_H
#define XIR_SOURCE_CLASS_ARRAY_ALLOCATIONS_H
static void source_class_array_allocations(void) {
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CLASS_ARRAY_FIXTURES};
    XrXirSourceRequest request={NULL,XR_CLASS_ARRAY_FIXTURES "/root.xr",&authority,NULL,NULL,NULL, XR_XIR_PROGRAM, NULL};
    size_t sites=allocation_case_begin(24);
    for(size_t pass=allocation_first();allocation_more(pass,sites);pass=allocation_next(pass)){
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=pass?pass-1:SIZE_MAX; source_fixture_compile_injected=false;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=allocation_source_check(&request,&result,&diagnostic);
        if(!pass){CHECK(status==XR_XIR_OK && result.checked && result.snapshot);sites=source_fixture_compile_attempts;CHECK(sites>0);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.status==XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);CHECK(!source_fixture_compile_live);
            if(pass)allocation_point(pass-1,XR_XIR_OUT_OF_MEMORY,true);
}
        allocation_case_end(sites);
    source_fixture_compile_fail_at=SIZE_MAX; source_fixture_compile_injected=false;printf("class Array source: %zu allocation failure sites; no partial Checked/source_fixture_compile_live metadata\n",sites);
}
#endif
