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
#include "xir_sdk_resource_test.h"
static size_t sdk_io_attempts,sdk_io_fail_at=SIZE_MAX;
static DWORD sdk_io_error=ERROR_READ_FAULT;
static bool sdk_short_read;
static bool sdk_io_fault(void) {
    if (sdk_io_attempts++!=sdk_io_fail_at) return false;
    SetLastError(sdk_io_error);return true;
}
static BOOL sdk_test_read(HANDLE file,LPVOID bytes,DWORD length,LPDWORD actual,LPOVERLAPPED overlapped) {
    if (sdk_io_fault()) return FALSE;
    if (sdk_short_read) {*actual=0;return TRUE;}
    return ReadFile(file,bytes,length,actual,overlapped);
}
#define CreateFileW(...) (sdk_io_fault() ? INVALID_HANDLE_VALUE : CreateFileW(__VA_ARGS__))
#define ReadFile sdk_test_read
#define GetFileInformationByHandle(...) (sdk_io_fault() ? FALSE : GetFileInformationByHandle(__VA_ARGS__))
#define GetFileInformationByHandleEx(...) (sdk_io_fault() ? FALSE : GetFileInformationByHandleEx(__VA_ARGS__))
#define GetFinalPathNameByHandleW(...) (sdk_io_fault() ? 0u : GetFinalPathNameByHandleW(__VA_ARGS__))
#define GetFileSizeEx(...) (sdk_io_fault() ? FALSE : GetFileSizeEx(__VA_ARGS__))
#define GetFileType(...) (sdk_io_fault() ? FILE_TYPE_UNKNOWN : GetFileType(__VA_ARGS__))
#define MultiByteToWideChar(...) (sdk_io_fault() ? 0 : MultiByteToWideChar(__VA_ARGS__))
#define WideCharToMultiByte(...) (sdk_io_fault() ? 0 : WideCharToMultiByte(__VA_ARGS__))
#include "toolchain/xr_xir_runtime_sdk.c"
#include "app/toolchain/xtc_xir_target.c"
#include "app/toolchain/xtc_xir_sysroot.c"
#include "app/toolchain/xtc_xir_images.c"
#undef CreateFileW
#undef ReadFile
#undef GetFileInformationByHandle
#undef GetFileInformationByHandleEx
#undef GetFinalPathNameByHandleW
#undef GetFileSizeEx
#undef GetFileType
#undef MultiByteToWideChar
#undef WideCharToMultiByte
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
    const char *first=NULL;CHECK(xr_xir_runtime_sdk_file(sdk,"lib/xray_xir_admission.lib",&first)==XR_XIR_SDK_OK);
    wchar_t path[32768];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,first,-1,path,32768));
    HANDLE writer=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,0,NULL);
    CHECK(writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    HANDLE replacer=CreateFileW(path,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,0,NULL);
    CHECK(replacer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    const char *missing=NULL;
    CHECK(xr_xir_runtime_sdk_file(sdk,"../lib/xray_xir_admission.lib",&missing)==XR_XIR_SDK_UNRESOLVED && !missing);
    CHECK(xr_xir_runtime_sdk_file(sdk,"LIB/xray_xir_admission.lib",&missing)==XR_XIR_SDK_UNRESOLVED && !missing);
}
static void sdk_actual_faults(const XrXirRuntimeSdkRequest *original) {
    size_t live=runtime_live,bytes=runtime_bytes;DWORD handles=sdk_handles();
    XrCompileResourceStats before=sdk_stats(original->resources);
    runtime_attempts=0;sdk_io_attempts=0;XrXirRuntimeSdk *sdk=NULL;
    CHECK(xr_xir_runtime_sdk_load(original,&sdk)==XR_XIR_SDK_OK && sdk);
    size_t sites=runtime_attempts,io_sites=sdk_io_attempts;
    XrXirRuntimeSdkFacts facts=*xr_xir_runtime_sdk_facts(sdk);
    XrCompileResourceStats used=sdk_stats(original->resources);
    CHECK(facts.file_count==sizeof(sdk_recipe_files)/sizeof(sdk_recipe_files[0]));
    /* Physical tracking includes the memory owner's private per-block header.
     * SDK metadata is its own requested resident capacity, excluding that header
     * and any already-live Target allocations on the shared owner. */
    CHECK(facts.metadata_bytes==runtime_bytes-bytes-(runtime_live-live)*sizeof(CompileAllocation));
    CHECK(used.held_bytes==runtime_bytes && used.held_bytes-before.held_bytes==runtime_bytes-bytes);
    uint64_t sdk_peak=sizeof(XrCompileResources)+used.held_bytes-before.held_bytes;
    CHECK(!memcmp(facts.identity,sdk_expected_identity,32));sdk_locked_files(sdk);
    xr_xir_runtime_sdk_free(sdk);
    CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    for (size_t at=0;at<sites;++at) {
        runtime_attempts=0;runtime_fail_at=at;sdk=NULL;
        XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(original,&sdk);runtime_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_SDK_OUT_OF_MEMORY && !sdk && runtime_attempts>at);
        CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    }
    for (unsigned minus=0;minus<2;++minus) {
        XrXirRuntimeSdkRequest request=*original;
        XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,sdk_peak-minus,true};
        request.resources=sdk_ledger(&exact);sdk=NULL;
        XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);
        CHECK(status==(minus ? XR_XIR_SDK_MEMORY_LIMIT : XR_XIR_SDK_OK));
        CHECK(minus ? !sdk : sdk!=NULL);xr_xir_runtime_sdk_free(sdk);
        xr_compile_resources_release(request.resources);
        CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    }
    for (size_t i=0;i<io_sites;++i) {
        for (unsigned mode=0;mode<2;++mode) {
            sdk_io_attempts=0;sdk_io_fail_at=i;sdk=NULL;
            sdk_io_error=mode ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT;
            CHECK(xr_xir_runtime_sdk_load(original,&sdk)==(mode ? XR_XIR_SDK_OUT_OF_MEMORY : XR_XIR_SDK_IO) && !sdk);
            sdk_io_fail_at=SIZE_MAX;
            CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
        }
    }
    sdk_io_attempts=0;sdk_io_fail_at=1;sdk_io_error=ERROR_NOT_ENOUGH_MEMORY;sdk=NULL;
    CHECK(xr_xir_runtime_sdk_load(original,&sdk)==XR_XIR_SDK_OUT_OF_MEMORY && !sdk);
    sdk_io_fail_at=SIZE_MAX;sdk_io_error=ERROR_READ_FAULT;
    CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    printf("SDK actual allocation sites=%zu IO/OOM sites=%zu resident-memory exact/minus1 PASS\n",sites,io_sites);
}
#include "xir_sdk_shared_resources.inc.c"
static void sdk_library_parser(void) {
    const char *cases[] = {"[\"kernel32\",\"bcrypt\",\"advapi32\",\"userenv\"]",
        "[\"kernel32\",\"bcrypt\",\"advapi32\"]",
        "[\"kernel32\",\"bcrypt\"]", "[\"kernel32\"]", "[\"bcrypt\"]",
        "[\"advapi32\"]", "[\"kernel32\",\"advapi32\"]", "[\"bcrypt\",\"advapi32\"]",
        "[\"bcrypt\",\"kernel32\"]", "[\"bcrypt\",\"kernel32\",\"advapi32\"]",
        "[\"kernel32\",\"advapi32\",\"bcrypt\"]", "[\"kernel32\",\"bcrypt\",\"bcrypt\"]",
        "[\"kernel32\",\"bcrypt\",\"user32\"]", "[]", "[\"kernel32\",\"Bcrypt\"]",
        "[\"kernel32\",\"bcrypt\",\"Advapi32\"]",
        "[\"kernel32\",\"bcrypt\",\"advapi32\",\"user32\"]",
        "[\"kernel32\",\"bcrypt\",\"advapi32\",\"advapi32\"]",
        "[\"kernel32\",\"bcrypt\",\"userenv\",\"advapi32\"]",
        "[\"bcrypt\",\"kernel32\",\"advapi32\",\"userenv\"]",
        "[\"kernel32\",\"advapi32\",\"bcrypt\",\"userenv\"]",
        "[\"kernel32\",\"bcrypt\",\"advapi32\",\"Userenv\"]",
        "[\"kernel32\",\"bcrypt\",\"advapi32\",\"userenv\",\"userenv\"]"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char bytes[128];size_t length = strlen(cases[i]);CHECK(length < sizeof(bytes));
        memcpy(bytes, cases[i], length + 1);
        XrJsonCursor json = xr_json_cursor_make(bytes, length);
        CHECK(sdk_json_libraries(&json) == (i == 0));
        if (i == 0) CHECK(json.status == XR_JSON_CURSOR_OK && json.cursor == json.end);
    }
}
int main(int argc,char **argv) {
    if ((argc!=3 && argc!=4) || (argc==4 && strcmp(argv[3],"status") &&
        strcmp(argv[3],"--shared-resources") && strcmp(argv[3],"--faults"))) {
        fputs("usage: sdk-test ROOT MANIFEST [status|--shared-resources|--faults]\n",stderr);return 2;
    }
    bool status_only=argc==4 && !strcmp(argv[3],"status");
    bool shared_only=argc==4 && !strcmp(argv[3],"--shared-resources");
    bool faults=argc==4 && !strcmp(argv[3],"--faults");
    size_t length=0;char *manifest=sdk_input(argv[2],&length);
    char *producer_root=xr_malloc(strlen(argv[1])+1);CHECK(producer_root);
    memcpy(producer_root,argv[1],strlen(argv[1])+1);
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirRuntimeSdkRequest request={producer_root,manifest,length,resources};
    DWORD initial_handles=sdk_handles();
    if (shared_only) {
        sdk_shared_resources(&request,false);
        xr_free(manifest);xr_free(producer_root);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes && sdk_handles()==initial_handles);
        puts("SDK/Target shared owner and target allocation/IO faults PASS");return 0;
    }
    if (!status_only) {
        sdk_library_parser();sdk_io_and_utf8();sdk_bounded_scanning();
        if (faults) sdk_shared_resources(&request,true);
        XrXirRuntimeSdkRequest too_large=request;too_large.manifest_length=XR_XIR_SDK_MANIFEST_LIMIT+1;
        XrXirRuntimeSdk *empty=NULL;
        CHECK(xr_xir_runtime_sdk_load(&too_large,&empty)==XR_XIR_SDK_BUDGET && !empty);
        XrXirRuntimeSdkRequest no_resources=request;no_resources.resources=NULL;
        CHECK(xr_xir_runtime_sdk_load(&no_resources,&empty)==XR_XIR_SDK_INVALID && !empty);
        XrXirRuntimeSdk *unchanged=(XrXirRuntimeSdk *)(uintptr_t)1;size_t before=runtime_attempts;
        CHECK(xr_xir_runtime_sdk_load(&request,&unchanged)==XR_XIR_SDK_INVALID &&
            unchanged==(XrXirRuntimeSdk *)(uintptr_t)1 && runtime_attempts==before);
    }
    XrXirRuntimeSdk *sdk=NULL;XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);
    if (!status_only) {
        CHECK(status==XR_XIR_SDK_OK && sdk);
        const char *existing=(const char *)(uintptr_t)1;
        XrCompileResourceStats before=sdk_stats(resources);
        CHECK(xr_xir_runtime_sdk_file(sdk,"ignored",&existing)==XR_XIR_SDK_INVALID &&
            existing==(const char *)(uintptr_t)1);
        XrCompileResourceStats after=sdk_stats(resources);
        CHECK(!memcmp(&before,&after,sizeof(before)));
    }
    xr_free(manifest);xr_free(producer_root);xr_compile_resources_release(resources);
    if (status_only) printf("%u",(unsigned)status);
    if (sdk) {
        const XrXirRuntimeSdkFacts *facts=xr_xir_runtime_sdk_facts(sdk);
        CHECK(xr_xir_runtime_sdk_root(sdk) && facts && facts->value_abi==XR_XIR_VALUE_ABI_VERSION);
        if (status_only) {
            printf(" %u ",facts->file_count);
            for (size_t i=0;i<32;++i) printf("%02x",facts->identity[i]);
        } else sdk_locked_files(sdk);
    }
    puts(status_only?"":"SDK actual bundle remains owned after producer retirement PASS");
    xr_xir_runtime_sdk_free(sdk);
    CHECK(!runtime_live && !runtime_bytes && sdk_handles()==initial_handles);return 0;
}
