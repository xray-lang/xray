/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_resource_test.h - Count physical ledger allocations at the real allocator
 */
#ifndef XIR_SDK_RESOURCE_TEST_H
#define XIR_SDK_RESOURCE_TEST_H
#include "base/xmalloc.h"
static void *sdk_fixture_malloc(size_t bytes) {return xr_malloc(bytes);}
static void sdk_fixture_free(void *pointer) {xr_free(pointer);}
typedef struct SdkAllocation { void *pointer; size_t bytes; } SdkAllocation;
static SdkAllocation sdk_allocations[4096];
static size_t runtime_attempts,runtime_fail_at=SIZE_MAX,runtime_live,runtime_bytes;
static void *sdk_test_malloc(size_t bytes) {
    if (runtime_attempts++==runtime_fail_at) return NULL;
    void *pointer=xr_malloc(bytes);if (!pointer) return NULL;
    for (size_t i=0;i<4096;++i) if (!sdk_allocations[i].pointer) {
        sdk_allocations[i]=(SdkAllocation){pointer,bytes};++runtime_live;runtime_bytes+=bytes;return pointer;
    }
    CHECK(false);return NULL;
}
static void sdk_test_free(void *pointer) {
    if (!pointer) return;
    for (size_t i=0;i<4096;++i) if (sdk_allocations[i].pointer==pointer) {
        --runtime_live;runtime_bytes-=sdk_allocations[i].bytes;sdk_allocations[i]=(SdkAllocation){0};
        xr_free(pointer);return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc sdk_test_malloc
#define xr_free sdk_test_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
#define xr_malloc sdk_fixture_malloc
#define xr_free sdk_fixture_free
static const XrCompileResourceLimits sdk_unlimited={UINT64_MAX,UINT64_MAX,UINT64_MAX};
static XrCompileResources *sdk_ledger(const XrCompileResourceLimits *limits) {
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(limits,&resources)==XR_COMPILE_RESOURCE_OK);return resources;
}
static XrCompileResourceStats sdk_stats(XrCompileResources *resources) {
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
#endif // XIR_SDK_RESOURCE_TEST_H
