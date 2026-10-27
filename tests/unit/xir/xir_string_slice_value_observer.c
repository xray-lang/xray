/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_string_slice_value_observer.c - Physical String slice refusal controls
 *
 * This translation unit observes the actual Value implementation. Compiler
 * held memory uses its public owner statistics, independently of runtime OOM.
 */
#include "base/xmalloc.h"
#include "xir/xxir_compile_domain.h"
#include <stdint.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"slice resources %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static size_t slice_calls,slice_live,slice_fail_at=SIZE_MAX;
static void *slice_malloc(size_t bytes) {
    if(slice_calls++==slice_fail_at) return NULL;
    void *value=xr_malloc(bytes);if(value) ++slice_live;return value;
}
static void slice_free(void *value) {
    if(value){CHECK(slice_live);--slice_live;}xr_free(value);
}
static void *slice_calloc(size_t count,size_t bytes) {
    if(slice_calls++==slice_fail_at) return NULL;
    void *value=xr_calloc(count,bytes);if(value) ++slice_live;return value;
}
static void *slice_realloc(void *value,size_t bytes) {
    if(slice_calls++==slice_fail_at) return NULL;
    bool empty=!value;void *replacement=xr_realloc(value,bytes);
    if(replacement && empty) ++slice_live;return replacement;
}
#undef xr_malloc
#undef xr_free
#undef xr_calloc
#undef xr_realloc
#define xr_malloc(bytes) slice_malloc(bytes)
#define xr_free(value) slice_free(value)
#define xr_calloc(count,bytes) slice_calloc(count,bytes)
#define xr_realloc(value,bytes) slice_realloc(value,bytes)
#include "xir/xxir_value.c"

static void slice_physical_oom(void) {
    for(size_t failure=0;failure<3;++failure) {
        XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
        XrXirValue input={0},output={0};
        CHECK(xr_xir_string_new(domain,"A\xc3\xa9\xe4\xb8\xad",6,&input)==XR_XIR_VALUE_OK);
        XrXirValueAdmission admission={.domain=domain,.work=UINT64_MAX};
        size_t before=slice_calls,live=slice_live;slice_fail_at=before+failure;
        XrXirValueStatus status=xr_xir_string_slice(&input,1,3,&admission,&output);
        slice_fail_at=SIZE_MAX;
        CHECK(status==(failure<2?XR_XIR_VALUE_OOM:XR_XIR_VALUE_OK));
        if(failure<2) CHECK(!output.type && !output.payload && slice_live==live);
        else CHECK(slice_calls-before==2 && output.type==XR_XIR_STRING);
        xr_xir_value_drop(&output);xr_xir_value_drop(&input);xr_xir_domain_drop(domain);CHECK(!slice_live);
    }
}
static uint64_t slice_held(XrCompileResourceLimits limits,XrXirValueStatus expected) {
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0},finished={0};CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirDomain *domain=NULL;CHECK(xr_xir_compile_domain_new(resources,UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValue input={0},output={0};CHECK(xr_xir_string_new(domain,"abcdef",6,&input)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.domain=domain};
    CHECK(xr_xir_string_slice(&input,1,3,&admission,&output)==expected);
    if(expected!=XR_XIR_VALUE_OK) CHECK(!output.type && !output.payload);
    xr_xir_value_drop(&output);xr_xir_value_drop(&input);xr_xir_domain_drop(domain);
    CHECK(xr_compile_resources_stats(resources,&finished)==XR_COMPILE_RESOURCE_OK);
    CHECK(finished.held_bytes==initial.held_bytes);xr_compile_resources_release(resources);
    return finished.peak_bytes;
}
static void slice_independent_storage(void) {
    const size_t size=UINT32_C(1048576);char *bytes=malloc(size);CHECK(bytes);memset(bytes,'x',size);
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValue input={0},output={0};CHECK(xr_xir_string_new(domain,bytes,size,&input)==XR_XIR_VALUE_OK);free(bytes);
    XrXirValueAdmission admission={.domain=domain,.work=UINT64_MAX};
    CHECK(xr_xir_string_slice(&input,1,2,&admission,&output)==XR_XIR_VALUE_OK);
    uint64_t before=xr_xir_domain_stats(domain).live_bytes;
    xr_xir_value_drop(&input);CHECK(xr_xir_domain_stats(domain).live_bytes<before-size);
    xr_xir_domain_drop(domain);
    const char *text=NULL;size_t length=0;CHECK(xr_xir_string_view(&output,&text,&length) && length==1 && text[0]=='x');
    xr_xir_value_drop(&output);CHECK(!slice_live);
}
static void slice_work_refusal(void) {
    XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(UINT64_MAX,&domain)==XR_XIR_VALUE_OK);
    XrXirValue input={0},output={0};CHECK(xr_xir_string_new(domain,"abcdef",6,&input)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={.domain=domain,.work=1};size_t calls=slice_calls,live=slice_live;
    CHECK(xr_xir_string_slice(&input,1,3,&admission,&output)==XR_XIR_VALUE_LIMIT);
    CHECK(!output.type && !output.payload && slice_calls==calls && slice_live==live);
    xr_xir_value_drop(&input);xr_xir_domain_drop(domain);CHECK(!slice_live);
}
void xr_test_string_slice_resources(void) {
    slice_physical_oom();slice_independent_storage();slice_work_refusal();
    uint64_t peak=slice_held((XrCompileResourceLimits){XR_COMPILE_MEMORY_ORDINARY,0,true},XR_XIR_VALUE_OK);
    CHECK(peak>1);
    (void)slice_held((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,peak-1,true},XR_XIR_VALUE_MEMORY_LIMIT);
    (void)slice_held((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,peak,true},XR_XIR_VALUE_OK);
    (void)slice_held((XrCompileResourceLimits){XR_COMPILE_MEMORY_FINITE,peak+1,true},XR_XIR_VALUE_OK);
    CHECK(!slice_live);printf("String slice physical two-site OOM, held %llu +/-1, independent storage and runtime refusal passed\n",(unsigned long long)peak);
}
