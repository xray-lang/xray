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
static void source_class_array_allocations(XrCompilerSession *session) {
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CLASS_ARRAY_FIXTURES};
    XrXirSourceRequest request={session,XR_CLASS_ARRAY_FIXTURES "/root.xr",&authority,NULL,NULL,NULL};
    size_t sites=0;
    for(size_t pass=0;pass<=sites;++pass){
        attempts=0;fail_at=pass?pass-1:SIZE_MAX;XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=xr_xir_source_check(&request,&result,&diagnostic);
        if(!pass){CHECK(status==XR_XIR_OK && result.checked && result.snapshot);sites=attempts;CHECK(sites>0);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY && diagnostic.status==XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        xr_xir_source_result_free(&result);CHECK(!live);
    }
    fail_at=SIZE_MAX;printf("class Array source: %zu allocation failure sites; no partial Checked/live metadata\n",sites);
}
#endif
