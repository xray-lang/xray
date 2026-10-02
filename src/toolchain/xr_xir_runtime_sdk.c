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
#include "../xir/xxir_checked.h"
#include "../xir/xxir_output.h"
#include "../shared/xr_error_core.h"
#include "../execution/xr_xir_host_execution.h"
#include "../base/xmalloc.h"
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
} SdkDirectory;
struct XrXirRuntimeSdk {
    XrXirSdkManifest manifest;
    XrXirRuntimeSdkFacts facts;
    SdkMemory *memory;
    SdkDirectory *directories;
    wchar_t *root_wide, *scratch;
    const char *root;
    uint64_t metadata_used, metadata_limit, remaining_work, initial_work;
    XrXirRuntimeSdkStatus status;
};
static bool sdk_fail(XrXirRuntimeSdk *sdk, XrXirRuntimeSdkStatus status) {
    if (sdk->status == XR_XIR_SDK_OK) sdk->status = status;
    return false;
}
static bool sdk_work(XrXirRuntimeSdk *sdk, uint64_t count) {
    if (sdk->status != XR_XIR_SDK_OK) return false;
    if (count > sdk->remaining_work) return sdk_fail(sdk, XR_XIR_SDK_BUDGET);
    sdk->remaining_work -= count; return true;
}
static void *sdk_allocate(XrXirRuntimeSdk *sdk, size_t bytes) {
    if (sdk->status != XR_XIR_SDK_OK) return NULL;
    if (bytes > SIZE_MAX-sizeof(SdkMemory) ||
        (uint64_t)bytes+sizeof(SdkMemory) > sdk->metadata_limit-sdk->metadata_used) {
        sdk_fail(sdk,XR_XIR_SDK_BUDGET);return NULL;
    }
    SdkMemory *memory=xr_malloc(sizeof(*memory)+bytes);
    if (!memory) {sdk_fail(sdk,XR_XIR_SDK_OUT_OF_MEMORY);return NULL;}
    sdk->metadata_used+=sizeof(*memory)+bytes;
    memory->next=sdk->memory;sdk->memory=memory;
    memset(memory+1,0,bytes);return memory+1;
}
#include "xr_xir_runtime_sdk_windows.inc.c"

static void sdk_facts_publish(XrXirRuntimeSdk *sdk) {
    const uint32_t *p=sdk->manifest.prefix;
    sdk->facts=(XrXirRuntimeSdkFacts){p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],
        p[10],p[11],p[12],p[13],p[14],p[15],p[16],sdk->manifest.file_count,
        sdk->manifest.bundle_bytes,sdk->metadata_used,sdk->initial_work-sdk->remaining_work,{0}};
    memcpy(sdk->facts.identity,sdk->manifest.digest,32);
}
XR_FUNC XrXirRuntimeSdkStatus xr_xir_runtime_sdk_load(const XrXirRuntimeSdkRequest *request,
    XrXirRuntimeSdk **output) {
    if (!request || !output || *output || !request->root || !request->manifest || !request->manifest_length)
        return XR_XIR_SDK_INVALID;
    if (request->manifest_length > XR_XIR_SDK_MANIFEST_LIMIT ||
        request->metadata_limit > XR_XIR_SDK_BUNDLE_BYTES || request->work_limit > XR_XIR_SDK_WORK_LIMIT ||
        request->metadata_limit < sizeof(XrXirRuntimeSdk)) return XR_XIR_SDK_BUDGET;
    XrXirRuntimeSdk *sdk=xr_calloc(1,sizeof(*sdk));
    if (!sdk) return XR_XIR_SDK_OUT_OF_MEMORY;
    sdk->metadata_used=sizeof(*sdk);sdk->metadata_limit=request->metadata_limit;
    sdk->initial_work=sdk->remaining_work=request->work_limit;
    char *manifest=sdk_allocate(sdk,request->manifest_length);
    if (manifest) {
        if (sdk_work(sdk,request->manifest_length)) {
            memcpy(manifest,request->manifest,request->manifest_length);
            sdk->status=sdk_json_parse(manifest,request->manifest_length,sdk->remaining_work,&sdk->manifest,NULL);
            sdk->remaining_work-=sdk->manifest.work_used;
        }
    }
    if (sdk->status==XR_XIR_SDK_OK) {
        SdkJson json={NULL,NULL,NULL,sdk->remaining_work,sdk->remaining_work,XR_XIR_SDK_OK};
        if (!sdk_identity(&json,&sdk->manifest)) sdk->status=json.status;
        sdk->remaining_work=json.remaining;
        if (sdk->status==XR_XIR_SDK_OK &&
            (memcmp(sdk->manifest.prefix,sdk_expected_prefix,sizeof(sdk_expected_prefix)) ||
             memcmp(sdk->manifest.digest,sdk_expected_identity,32))) sdk_fail(sdk,XR_XIR_SDK_INVALID);
    }
    if (sdk->status==XR_XIR_SDK_OK) sdk_files_admit(sdk,request->root);
    XrXirRuntimeSdkStatus status=sdk->status;
    if (status!=XR_XIR_SDK_OK) {xr_xir_runtime_sdk_free(sdk);return status;}
    sdk_facts_publish(sdk);*output=sdk;return XR_XIR_SDK_OK;
}
XR_FUNC const XrXirRuntimeSdkFacts *xr_xir_runtime_sdk_facts(const XrXirRuntimeSdk *sdk) {
    return sdk ? &sdk->facts : NULL;
}
XR_FUNC const char *xr_xir_runtime_sdk_root(const XrXirRuntimeSdk *sdk) {
    return sdk ? sdk->root : NULL;
}
XR_FUNC const char *xr_xir_runtime_sdk_file(const XrXirRuntimeSdk *sdk, const char *relative_path) {
    if (!sdk || !relative_path) return NULL;
    for (uint32_t i=0;i<sdk->manifest.file_count;++i)
        if (!strcmp(relative_path,sdk->manifest.files[i].path)) return sdk->manifest.files[i].absolute_path;
    return NULL;
}
XR_FUNC void xr_xir_runtime_sdk_free(XrXirRuntimeSdk *sdk) {
    if (!sdk) return;
    sdk_files_close(sdk);
    while (sdk->memory) {SdkMemory *next=sdk->memory->next;xr_free(sdk->memory);sdk->memory=next;}
    xr_free(sdk);
}
