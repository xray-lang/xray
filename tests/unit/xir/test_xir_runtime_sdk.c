/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_runtime_sdk.c - Actual bundle admission, ownership and fault boundaries
 */
#include "toolchain/xr_xir_runtime_sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_runtime_allocations.h"
static size_t sdk_io_attempts,sdk_io_fail_at=SIZE_MAX;
static DWORD sdk_io_error=ERROR_READ_FAULT;
static bool sdk_io_fault(void) {
    if (sdk_io_attempts++!=sdk_io_fail_at) return false;
    SetLastError(sdk_io_error);return true;
}
static BOOL sdk_test_read(HANDLE file,LPVOID bytes,DWORD length,LPDWORD actual,LPOVERLAPPED overlapped) {
    if (sdk_io_fault()) return FALSE;
    return ReadFile(file,bytes,length,actual,overlapped);
}
#define CreateFileW(...) (sdk_io_fault() ? INVALID_HANDLE_VALUE : CreateFileW(__VA_ARGS__))
#define ReadFile sdk_test_read
#define GetFileInformationByHandle(...) (sdk_io_fault() ? FALSE : GetFileInformationByHandle(__VA_ARGS__))
#define GetFinalPathNameByHandleW(...) (sdk_io_fault() ? 0u : GetFinalPathNameByHandleW(__VA_ARGS__))
#define GetFileSizeEx(...) (sdk_io_fault() ? FALSE : GetFileSizeEx(__VA_ARGS__))
#define GetFileType(...) (sdk_io_fault() ? FILE_TYPE_UNKNOWN : GetFileType(__VA_ARGS__))
#define MultiByteToWideChar(...) (sdk_io_fault() ? 0 : MultiByteToWideChar(__VA_ARGS__))
#define WideCharToMultiByte(...) (sdk_io_fault() ? 0 : WideCharToMultiByte(__VA_ARGS__))
#include "toolchain/xr_xir_runtime_sdk.c"
#undef CreateFileW
#undef ReadFile
#undef GetFileInformationByHandle
#undef GetFinalPathNameByHandleW
#undef GetFileSizeEx
#undef GetFileType
#undef MultiByteToWideChar
#undef WideCharToMultiByte
#include "sdk_identity_golden.h"

static void sdk_known_bytes(void) {
    XrXirSdkManifest manifest={0};const uint32_t prefix[]={2,22,58,17,21,26,1,1,1,11,2,0,1,0,3,1,1};
    memcpy(manifest.prefix,prefix,sizeof(prefix));
    manifest.target_triple="x86_64-windows-msvc";manifest.abi_recipe="xray:xir-runtime-abi-measurements:v1";
    manifest.closure_recipe="xray:xir-runtime-recipe:windows-x86_64-hosted:v1";
    manifest.file_count=1;manifest.files[0]=(XrXirSdkFile){"lib/test.lib",5,2,{0},NULL,NULL};
    memcpy(manifest.files[0].digest,sdk_kat_file_digest,32);
    SdkJson json={NULL,NULL,NULL,sizeof(sdk_kat_preimage),sizeof(sdk_kat_preimage),XR_XIR_SDK_OK};
    CHECK(sdk_identity(&json,&manifest) && !json.remaining && !memcmp(manifest.digest,sdk_kat_digest,32));
    uint8_t hash[32];xr_sha256(sdk_kat_preimage,sizeof(sdk_kat_preimage),hash);
    CHECK(!memcmp(hash,sdk_kat_digest,32));
    memset(manifest.digest,0,32);--json.initial;json.remaining=json.initial;
    CHECK(!sdk_identity(&json,&manifest) && json.status==XR_XIR_SDK_BUDGET);
    for (size_t i=0;i<32;++i) CHECK(!manifest.digest[i]);
}

static char *sdk_input(const char *path,size_t *length) {
    FILE *file=fopen(path,"rb");CHECK(file && !fseek(file,0,SEEK_END));
    long size=ftell(file);CHECK(size>0 && (uint64_t)size<=XR_XIR_SDK_MANIFEST_LIMIT && !fseek(file,0,SEEK_SET));
    char *bytes=xr_malloc((size_t)size+1);CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size && !fclose(file));
    bytes[size]=0;*length=(size_t)size;return bytes;
}
static DWORD sdk_handles(void) {
    DWORD count=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&count));return count;
}
static void sdk_locked_files(const XrXirRuntimeSdk *sdk) {
    const char *first=xr_xir_runtime_sdk_file(sdk,"lib/xray_xir_admission.lib");CHECK(first);
    wchar_t path[32768];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,first,-1,path,32768));
    HANDLE writer=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,0,NULL);
    CHECK(writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    HANDLE replacer=CreateFileW(path,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,0,NULL);
    CHECK(replacer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    CHECK(!xr_xir_runtime_sdk_file(sdk,"../lib/xray_xir_admission.lib"));
    CHECK(!xr_xir_runtime_sdk_file(sdk,"LIB/xray_xir_admission.lib"));
}
static void sdk_actual_faults(const XrXirRuntimeSdkRequest *original) {
    size_t live=runtime_live,bytes=runtime_bytes;DWORD handles=sdk_handles();
    runtime_attempts=0;sdk_io_attempts=0;XrXirRuntimeSdk *sdk=NULL;
    CHECK(xr_xir_runtime_sdk_load(original,&sdk)==XR_XIR_SDK_OK && sdk);
    size_t sites=runtime_attempts,io_sites=sdk_io_attempts;
    XrXirRuntimeSdkFacts facts=*xr_xir_runtime_sdk_facts(sdk);
    CHECK(facts.file_count==sizeof(sdk_recipe_files)/sizeof(sdk_recipe_files[0]));
    CHECK(facts.metadata_bytes==runtime_bytes-bytes && facts.work_used>facts.file_bytes*2);
    CHECK(!memcmp(facts.identity,sdk_expected_identity,32));sdk_locked_files(sdk);
    xr_xir_runtime_sdk_free(sdk);
    CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    for (size_t at=0;at<sites;++at) {
        runtime_attempts=0;runtime_fail_at=at;sdk=NULL;
        XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(original,&sdk);runtime_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_SDK_OUT_OF_MEMORY && !sdk && runtime_attempts>at);
        CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    }
    for (size_t pass=0;pass<3;++pass) {
        XrXirRuntimeSdkRequest request=*original;
        request.metadata_limit=facts.metadata_bytes-(pass==1);
        request.work_limit=facts.work_used-(pass==2);sdk=NULL;
        XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);
        CHECK(status==(pass ? XR_XIR_SDK_BUDGET : XR_XIR_SDK_OK));
        CHECK(pass ? !sdk : sdk!=NULL);xr_xir_runtime_sdk_free(sdk);
        CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    }
    for (size_t i=0;i<io_sites;++i) {
        sdk_io_attempts=0;sdk_io_fail_at=i;sdk=NULL;
        CHECK(xr_xir_runtime_sdk_load(original,&sdk)==XR_XIR_SDK_IO && !sdk);
        sdk_io_fail_at=SIZE_MAX;
        CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    }
    sdk_io_attempts=0;sdk_io_fail_at=1;sdk_io_error=ERROR_NOT_ENOUGH_MEMORY;sdk=NULL;
    CHECK(xr_xir_runtime_sdk_load(original,&sdk)==XR_XIR_SDK_OUT_OF_MEMORY && !sdk);
    sdk_io_fail_at=SIZE_MAX;sdk_io_error=ERROR_READ_FAULT;
    CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    printf("SDK actual allocation sites=%zu IO sites=%zu exact/minus1 bytes/work/handle refunds PASS\n",sites,io_sites);
}
int main(int argc,char **argv) {
    sdk_known_bytes();
    CHECK(argc==3 || argc==4);size_t length=0;char *manifest=sdk_input(argv[2],&length);
    XrXirRuntimeSdkRequest request={argv[1],manifest,length,16777216,XR_XIR_SDK_WORK_LIMIT};
    if (argc==4) {
        XrXirRuntimeSdk *sdk=NULL;XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);
        printf("%u",(unsigned)status);
        if (sdk) {
            const XrXirRuntimeSdkFacts *facts=xr_xir_runtime_sdk_facts(sdk);
            printf(" %u ",facts->file_count);
            for (size_t i=0;i<32;++i) printf("%02x",facts->identity[i]);
        }
        puts("");xr_xir_runtime_sdk_free(sdk);xr_free(manifest);
        CHECK(!runtime_live && !runtime_bytes);return 0;
    }
    sdk_actual_faults(&request);
    XrXirRuntimeSdkRequest too_large=request;too_large.manifest_length=XR_XIR_SDK_MANIFEST_LIMIT+1;
    XrXirRuntimeSdk *empty=NULL;CHECK(xr_xir_runtime_sdk_load(&too_large,&empty)==XR_XIR_SDK_BUDGET && !empty);
    XrXirRuntimeSdk *unchanged=(XrXirRuntimeSdk *)(uintptr_t)1;size_t before=runtime_attempts;
    CHECK(xr_xir_runtime_sdk_load(&request,&unchanged)==XR_XIR_SDK_INVALID &&
        unchanged==(XrXirRuntimeSdk *)(uintptr_t)1 && runtime_attempts==before);
    char *producer_root=xr_malloc(strlen(request.root)+1);CHECK(producer_root);
    memcpy(producer_root,request.root,strlen(request.root)+1);request.root=producer_root;
    XrXirRuntimeSdk *sdk=NULL;CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
    xr_free(manifest);xr_free(producer_root);
    CHECK(xr_xir_runtime_sdk_root(sdk) && xr_xir_runtime_sdk_facts(sdk)->value_abi==17);
    sdk_locked_files(sdk);xr_xir_runtime_sdk_free(sdk);CHECK(!runtime_live && !runtime_bytes);
    puts("SDK producer destroyed, immutable same-source facts and locked actual bundle PASS");return 0;
}
