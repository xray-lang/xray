/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk.c - Closed same-source manifest and physical file authority
 *
 * KEY CONCEPT:
 *   Published paths keep read-only handles and ancestors alive until consumption.
 */
#include "xr_xir_runtime_sdk_internal.h"
#include "../xir/xxir_program.h"
#include "../xir/xxir_atomic.h"
#include "../xir/xxir_nominal.h"
#include "../xir/xxir_checked.h"
#include "../xir/xxir_output.h"
#include "../shared/xr_error_core.h"
#include "../execution/xr_xir_host_execution.h"
#include "../base/xsha256.h"
#include "../base/xchecks.h"
#include <limits.h>
#include <float.h>
#include <string.h>
#include <wchar.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#endif

#define XR_XIR_SDK_ABI(id, value) {id, (uint32_t)(value)},
static const XrXirSdkAbiField sdk_abi_fields[] = {
#include "xr_xir_runtime_sdk_abi.def"
};
#undef XR_XIR_SDK_ABI
#include "sdk_recipe.inc.c"
#include "sdk_generated.h"
#include "xr_xir_runtime_sdk_json.inc.c"
#include "xr_xir_runtime_sdk_identity.inc.c"

typedef struct SdkMemory {
    struct SdkMemory *next;
    uint64_t alignment;
} SdkMemory;
typedef struct SdkDirectory {
    struct SdkDirectory *next;
    void *handle;
    wchar_t *path;
    size_t length;
} SdkDirectory;
struct XrXirRuntimeSdk {
    XrXirSdkManifest manifest;
    XrXirRuntimeSdkFacts facts;
    SdkMemory *memory;
    SdkDirectory *directories;
    wchar_t *root_wide, *scratch;
    const char *root;
    size_t root_length, scratch_length;
    XrCompileResources *resources;
    XrXirRuntimeSdkStatus status;
};
static bool sdk_fail(XrXirRuntimeSdk *sdk, XrXirRuntimeSdkStatus status) {
    if (sdk->status == XR_XIR_SDK_OK) sdk->status = status;
    return false;
}
static bool sdk_work(XrXirRuntimeSdk *sdk, uint64_t count) {
    if (sdk->status != XR_XIR_SDK_OK) return false;
    XrXirRuntimeSdkStatus status=sdk_resource_status(xr_compile_resources_work(sdk->resources,count));
    return status==XR_XIR_SDK_OK || sdk_fail(sdk,status);
}
static void *sdk_allocate(XrXirRuntimeSdk *sdk, size_t bytes) {
    if (sdk->status != XR_XIR_SDK_OK) return NULL;
    if (bytes > SIZE_MAX-sizeof(SdkMemory)) {
        sdk_fail(sdk,XR_XIR_SDK_BUDGET);return NULL;
    }
    void *allocation=NULL;
    XrXirRuntimeSdkStatus status=sdk_resource_status(xr_compile_resources_calloc(
        sdk->resources,1,sizeof(SdkMemory)+bytes,&allocation));
    if (status!=XR_XIR_SDK_OK) {sdk_fail(sdk,status);return NULL;}
    SdkMemory *memory=allocation;
    memory->next=sdk->memory;sdk->memory=memory;
    return memory+1;
}
#include "xr_xir_runtime_sdk_windows.inc.c"

static void sdk_facts_publish(XrXirRuntimeSdk *sdk,const XrCompileResourceStats *before) {
    const uint32_t *p=sdk->manifest.prefix;
    XrCompileResourceStats after;
    XR_CHECK(xr_compile_resources_stats(sdk->resources,&after)==XR_COMPILE_RESOURCE_OK,"Valid SDK ledger");
    sdk->facts=(XrXirRuntimeSdkFacts){p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],
        p[10],p[11],p[12],p[13],p[14],p[15],p[16],sdk->manifest.file_count,
        sdk->manifest.bundle_bytes,after.allocated_bytes-before->allocated_bytes,after.work-before->work,{0}};
    memcpy(sdk->facts.identity,sdk->manifest.digest,32);
}
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_load(const XrXirRuntimeSdkRequest *request,
    XrXirRuntimeSdk **output) {
    if (!request || !output || *output || !request->root || !request->manifest ||
        !request->manifest_length || !request->resources)
        return XR_XIR_SDK_INVALID;
    if (request->manifest_length > XR_XIR_SDK_MANIFEST_LIMIT) return XR_XIR_SDK_BUDGET;
    XrCompileResourceStats before;
    XrXirRuntimeSdkStatus status=sdk_resource_status(xr_compile_resources_stats(request->resources,&before));
    if (status!=XR_XIR_SDK_OK) return status;
    void *allocation=NULL;
    status=sdk_resource_status(xr_compile_resources_calloc(request->resources,1,sizeof(XrXirRuntimeSdk),&allocation));
    if (status!=XR_XIR_SDK_OK) return status;
    XrXirRuntimeSdk *sdk=allocation;
    sdk->resources=request->resources;
    char *manifest=sdk_allocate(sdk,request->manifest_length);
    if (manifest) {
        if (sdk_work(sdk,request->manifest_length)) {
            memcpy(manifest,request->manifest,request->manifest_length);
            sdk->status=sdk_json_parse(manifest,request->manifest_length,sdk->resources,&sdk->manifest,NULL);
        }
    }
    if (sdk->status==XR_XIR_SDK_OK) {
        XrJsonCursor json=xr_json_cursor_make(NULL,0,sdk->resources,sdk_cursor_charge);
        if (!sdk_identity(&json,&sdk->manifest)) sdk->status=sdk_cursor_status(json.status);
        if (sdk_work(sdk,sizeof(sdk_expected_prefix)) &&
            memcmp(sdk->manifest.prefix,sdk_expected_prefix,sizeof(sdk_expected_prefix))) sdk_fail(sdk,XR_XIR_SDK_INVALID);
        if (sdk_work(sdk,32) && memcmp(sdk->manifest.digest,sdk_expected_identity,32)) sdk_fail(sdk,XR_XIR_SDK_INVALID);
    }
    if (sdk->status==XR_XIR_SDK_OK) sdk_files_admit(sdk,request->root);
    if (sdk->status==XR_XIR_SDK_OK) sdk_work(sdk,sizeof(sdk->facts)+32);
    status=sdk->status;
    if (status!=XR_XIR_SDK_OK) {xr_xir_runtime_sdk_free(sdk);return status;}
    sdk_facts_publish(sdk,&before);*output=sdk;return XR_XIR_SDK_OK;
}
XR_FUNC const XrXirRuntimeSdkFacts *xr_xir_runtime_sdk_facts(const XrXirRuntimeSdk *sdk) {
    return sdk ? &sdk->facts : NULL;
}
XR_FUNC XrCompileResources *xr_xir_runtime_sdk_resources(const XrXirRuntimeSdk *sdk) {
    return sdk ? sdk->resources : NULL;
}
XR_FUNC const char *xr_xir_runtime_sdk_root(const XrXirRuntimeSdk *sdk) {
    return sdk ? sdk->root : NULL;
}
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_file(const XrXirRuntimeSdk *sdk,
    const char *relative_path,const char **output) {
    if (!sdk || !relative_path || !output || *output) return XR_XIR_SDK_INVALID;
    XrJsonCursor json=xr_json_cursor_make(NULL,0,sdk->resources,sdk_cursor_charge);
    for (uint32_t i=0;i<sdk->manifest.file_count;++i) {
        if (xr_json_cursor_equal(&json,relative_path,sdk->manifest.files[i].path)) {
            *output=sdk->manifest.files[i].absolute_path;return XR_XIR_SDK_OK;
        }
        if (json.status!=XR_JSON_CURSOR_OK) return sdk_cursor_status(json.status);
    }
    return XR_XIR_SDK_UNRESOLVED;
}
XR_FUNC void xr_xir_runtime_sdk_free(XrXirRuntimeSdk *sdk) {
    if (!sdk) return;
    sdk_files_close(sdk);
    while (sdk->memory) {SdkMemory *next=sdk->memory->next;xr_compile_resources_free(sdk->memory);sdk->memory=next;}
    xr_compile_resources_free(sdk);
}
