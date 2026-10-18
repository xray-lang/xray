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
static const XrCompileResourceLimits sdk_measurement_limits={
    UINT64_C(4294967296),UINT64_C(67108864),UINT64_C(68719476736)};
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
#include "sdk_identity_golden.h"
#define current_sdk_kat_preimage previous66_sdk_kat_preimage
#define current_sdk_kat_file_digest previous66_sdk_kat_file_digest
#define current_sdk_kat_digest previous66_sdk_kat_digest
#include "sdk_previous66_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous67_sdk_kat_preimage
#define current_sdk_kat_file_digest previous67_sdk_kat_file_digest
#define current_sdk_kat_digest previous67_sdk_kat_digest
#include "sdk_previous67_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous68_sdk_kat_preimage
#define current_sdk_kat_file_digest previous68_sdk_kat_file_digest
#define current_sdk_kat_digest previous68_sdk_kat_digest
#include "sdk_previous68_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous69_sdk_kat_preimage
#define current_sdk_kat_file_digest previous69_sdk_kat_file_digest
#define current_sdk_kat_digest previous69_sdk_kat_digest
#include "sdk_previous69_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous70_sdk_kat_preimage
#define current_sdk_kat_file_digest previous70_sdk_kat_file_digest
#define current_sdk_kat_digest previous70_sdk_kat_digest
#include "sdk_previous70_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous71_sdk_kat_preimage
#define current_sdk_kat_file_digest previous71_sdk_kat_file_digest
#define current_sdk_kat_digest previous71_sdk_kat_digest
#include "sdk_previous71_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous72_sdk_kat_preimage
#define current_sdk_kat_file_digest previous72_sdk_kat_file_digest
#define current_sdk_kat_digest previous72_sdk_kat_digest
#include "sdk_previous72_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous_value23_sdk_kat_preimage
#define current_sdk_kat_file_digest previous_value23_sdk_kat_file_digest
#define current_sdk_kat_digest previous_value23_sdk_kat_digest
#include "sdk_previous_value23_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous_value24_sdk_kat_preimage
#define current_sdk_kat_file_digest previous_value24_sdk_kat_file_digest
#define current_sdk_kat_digest previous_value24_sdk_kat_digest
#include "sdk_previous_value24_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous_call32_sdk_kat_preimage
#define current_sdk_kat_file_digest previous_call32_sdk_kat_file_digest
#define current_sdk_kat_digest previous_call32_sdk_kat_digest
#include "sdk_previous_call32_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous_semantic75_sdk_kat_preimage
#define current_sdk_kat_file_digest previous_semantic75_sdk_kat_file_digest
#define current_sdk_kat_digest previous_semantic75_sdk_kat_digest
#include "sdk_previous_semantic75_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#include "sdk_current_identity_golden.h"
#define current_sdk_kat_preimage previous_program34_sdk_kat_preimage
#define current_sdk_kat_file_digest previous_program34_sdk_kat_file_digest
#define current_sdk_kat_digest previous_program34_sdk_kat_digest
#include "sdk_previous_program34_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest

#define current_sdk_kat_preimage previous73_sdk_kat_preimage
#define current_sdk_kat_file_digest previous73_sdk_kat_file_digest
#define current_sdk_kat_digest previous73_sdk_kat_digest
#include "sdk_previous73_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previous74_sdk_kat_preimage
#define current_sdk_kat_file_digest previous74_sdk_kat_file_digest
#define current_sdk_kat_digest previous74_sdk_kat_digest
#include "sdk_previous74_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previousctfe75_sdk_kat_preimage
#define current_sdk_kat_file_digest previousctfe75_sdk_kat_file_digest
#define current_sdk_kat_digest previousctfe75_sdk_kat_digest
#include "sdk_previous_ctfe75_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest
#define current_sdk_kat_preimage previouschannel76_sdk_kat_preimage
#define current_sdk_kat_file_digest previouschannel76_sdk_kat_file_digest
#define current_sdk_kat_digest previouschannel76_sdk_kat_digest
#include "sdk_previous_channel76_identity_golden.h"
#undef current_sdk_kat_preimage
#undef current_sdk_kat_file_digest
#undef current_sdk_kat_digest

static void sdk_call32_prefix(XrCompileResources *resources,const XrXirSdkManifest *current) {
    uint8_t hash[32];xr_sha256(previous_call32_sdk_kat_preimage,sizeof(previous_call32_sdk_kat_preimage),hash);
    CHECK(sizeof(previous_call32_sdk_kat_preimage)==3186 && !memcmp(hash,previous_call32_sdk_kat_digest,32));
    CHECK(!memcmp(previous_call32_sdk_kat_file_digest,current_sdk_kat_file_digest,32));
    XrXirSdkManifest prior=*current;prior.prefix[2]=75;prior.prefix[4]=32;memcpy(prior.digest,previous_call32_sdk_kat_digest,32);
    XrXirSdkManifest saved=prior;size_t attempts=runtime_attempts;XrCompileResourceStats before=sdk_stats(resources);
    XrJsonCursor json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    CHECK(!sdk_json_prefix(&json,&prior) && json.status==XR_JSON_CURSOR_INVALID);
    CHECK(!memcmp(&prior,&saved,sizeof(prior)) && runtime_attempts==attempts);
    XrCompileResourceStats after=sdk_stats(resources);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
}
static void sdk_semantic75_prefix(XrCompileResources *resources,const XrXirSdkManifest *current) {
    uint8_t hash[32];xr_sha256(previous_semantic75_sdk_kat_preimage,sizeof(previous_semantic75_sdk_kat_preimage),hash);
    CHECK(sizeof(previous_semantic75_sdk_kat_preimage)==3194 && !memcmp(hash,previous_semantic75_sdk_kat_digest,32));
    CHECK(!memcmp(previous_semantic75_sdk_kat_file_digest,current_sdk_kat_file_digest,32));
    XrXirSdkManifest prior=*current;prior.prefix[2]=75;memcpy(prior.digest,previous_semantic75_sdk_kat_digest,32);
    XrXirSdkManifest saved=prior;size_t attempts=runtime_attempts;XrCompileResourceStats before=sdk_stats(resources);
    XrJsonCursor json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    CHECK(!sdk_json_prefix(&json,&prior) && json.status==XR_JSON_CURSOR_INVALID);
    CHECK(!memcmp(&prior,&saved,sizeof(prior)) && runtime_attempts==attempts);
    XrCompileResourceStats after=sdk_stats(resources);
    CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes && after.live_bytes==before.live_bytes);
}
static void sdk_combined_previous_prefix(XrCompileResources *resources,XrXirSdkManifest *manifest,const uint32_t *prefix) {
    XrJsonCursor json;
    const uint32_t predecessors[][5]={{28,73,24,29,30},{29,74,24,29,30},{30,75,24,29,31},{28,76,29,33,35}};
    const uint8_t *old_images[]={previous73_sdk_kat_preimage,previous74_sdk_kat_preimage,previousctfe75_sdk_kat_preimage,previouschannel76_sdk_kat_preimage};
    const size_t old_sizes[]={sizeof(previous73_sdk_kat_preimage),sizeof(previous74_sdk_kat_preimage),sizeof(previousctfe75_sdk_kat_preimage),sizeof(previouschannel76_sdk_kat_preimage)};
    const uint8_t *old_digests[]={previous73_sdk_kat_digest,previous74_sdk_kat_digest,previousctfe75_sdk_kat_digest,previouschannel76_sdk_kat_digest};
    for(unsigned prior=0;prior<4;++prior) {
        uint8_t old_hash[32];xr_sha256(old_images[prior],old_sizes[prior],old_hash);
        CHECK(!memcmp(old_hash,old_digests[prior],32));
        memcpy(manifest->prefix,prefix,sizeof(manifest->prefix));
        memcpy(manifest->prefix+1,predecessors[prior],sizeof(predecessors[prior]));
        memcpy(manifest->digest,old_digests[prior],32);
        XrXirSdkManifest saved=*manifest;size_t attempts=runtime_attempts;
        json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
        CHECK(!sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_INVALID);
        CHECK(!memcmp(manifest,&saved,sizeof(manifest)) && runtime_attempts==attempts);
    }
    memcpy(manifest->prefix,prefix,sizeof(manifest->prefix));
}

static void sdk_known_bytes(void) {
    _Static_assert(XR_XIR_CHECKED_SCHEMA==30 && XR_XIR_CHECKED_CONTRACT==77 &&
        XR_XIR_VALUE_ABI_VERSION==29 && XR_XIR_CALL_ABI_VERSION==33 && XR_XIR_PROGRAM_ABI_VERSION==35,
        "SDK known bytes require the independently framed public contract");
    uint8_t old_hash[32];xr_sha256(sdk_kat_preimage,sizeof(sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,sdk_kat_digest,32));
    xr_sha256((const uint8_t *)"hi",2,old_hash);CHECK(!memcmp(old_hash,sdk_kat_file_digest,32));
    _Static_assert(sizeof(previous66_sdk_kat_preimage)==3098,"complete previous66 SDK preimage");
    xr_sha256(previous66_sdk_kat_preimage,sizeof(previous66_sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,previous66_sdk_kat_digest,32));
    const size_t semantic_offset=sizeof("xray:xir-runtime-sdk:v1")-1+2*sizeof(uint32_t);
    _Static_assert(sizeof(previous67_sdk_kat_preimage)==3098,"complete previous67 SDK preimage");
    xr_sha256(previous67_sdk_kat_preimage,sizeof(previous67_sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,previous67_sdk_kat_digest,32));
    _Static_assert(sizeof(previous68_sdk_kat_preimage)==3098,"complete previous68 SDK preimage");
    xr_sha256(previous68_sdk_kat_preimage,sizeof(previous68_sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,previous68_sdk_kat_digest,32));
    _Static_assert(sizeof(previous69_sdk_kat_preimage)==3098,"complete previous69 SDK preimage");
    xr_sha256(previous69_sdk_kat_preimage,sizeof(previous69_sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,previous69_sdk_kat_digest,32));
    _Static_assert(sizeof(previous70_sdk_kat_preimage)==3098 && sizeof(current_sdk_kat_preimage)==3186,
        "complete current and previous70 SDK preimages");
    xr_sha256(previous70_sdk_kat_preimage,sizeof(previous70_sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,previous70_sdk_kat_digest,32));
    _Static_assert(sizeof(previous71_sdk_kat_preimage)==3098,"complete previous71 SDK preimage");
    xr_sha256(previous71_sdk_kat_preimage,sizeof(previous71_sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,previous71_sdk_kat_digest,32));
    _Static_assert(sizeof(previous_value23_sdk_kat_preimage)==3098,"complete retired Value23 SDK preimage");
    xr_sha256(previous_value23_sdk_kat_preimage,sizeof(previous_value23_sdk_kat_preimage),old_hash);
    CHECK(!memcmp(old_hash,previous_value23_sdk_kat_digest,32));
    const size_t wire_offset=semantic_offset-sizeof(uint32_t);
    CHECK(previous71_sdk_kat_preimage[wire_offset]==26 && previous71_sdk_kat_preimage[semantic_offset]==71);
    CHECK(!memcmp(previous71_sdk_kat_preimage,previous72_sdk_kat_preimage,wire_offset));
    CHECK(!memcmp(previous71_sdk_kat_preimage+semantic_offset+4,previous72_sdk_kat_preimage+semantic_offset+4,
        sizeof(previous72_sdk_kat_preimage)-semantic_offset-4));
    CHECK(previous70_sdk_kat_preimage[wire_offset]==25 && previous72_sdk_kat_preimage[wire_offset]==27);
    CHECK(!memcmp(previous70_sdk_kat_preimage,previous72_sdk_kat_preimage,wire_offset));
    CHECK(!memcmp(previous70_sdk_kat_preimage+semantic_offset+4,previous72_sdk_kat_preimage+semantic_offset+4,
        sizeof(previous72_sdk_kat_preimage)-semantic_offset-4));
    CHECK(previous66_sdk_kat_preimage[semantic_offset]==66 && previous67_sdk_kat_preimage[semantic_offset]==67 &&
        previous68_sdk_kat_preimage[semantic_offset]==68 && previous69_sdk_kat_preimage[semantic_offset]==69 &&
        previous70_sdk_kat_preimage[semantic_offset]==70 && previous72_sdk_kat_preimage[semantic_offset]==72);
    xr_sha256((const uint8_t *)"hi",2,old_hash);CHECK(!memcmp(old_hash,previous66_sdk_kat_file_digest,32) &&
        !memcmp(old_hash,previous67_sdk_kat_file_digest,32) && !memcmp(old_hash,previous68_sdk_kat_file_digest,32) && !memcmp(old_hash,previous69_sdk_kat_file_digest,32) && !memcmp(old_hash,previous70_sdk_kat_file_digest,32) && !memcmp(old_hash,previous71_sdk_kat_file_digest,32));
    XrXirSdkManifest manifest={0};const uint32_t prefix[]={2,30,77,29,33,35,1,1,1,11,2,0,1,0,3,1,1};
    memcpy(manifest.prefix,prefix,sizeof(prefix));
    manifest.target_triple="x86_64-windows-msvc";manifest.abi_recipe="xray:xir-runtime-abi-measurements:v1";
    manifest.closure_recipe="xray:xir-runtime-recipe:windows-x86_64-hosted:v1";
    manifest.file_count=1;manifest.files[0]=(XrXirSdkFile){"lib/test.lib",5,2,{0},NULL,NULL};
    memcpy(manifest.files[0].digest,current_sdk_kat_file_digest,32);
    XrCompileResources *resources=sdk_ledger(&sdk_measurement_limits);
    XrJsonCursor json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    CHECK(sdk_identity(&json,&manifest) && !memcmp(manifest.digest,current_sdk_kat_digest,32));
    uint64_t work=sdk_stats(resources).work;
    const uint64_t encoded_integers=4*(17+3+1+2*(sizeof(sdk_abi_fields)/sizeof(sdk_abi_fields[0]))+1+1+1+1+1)+8;
    const uint64_t scanned_strings=sizeof("x86_64-windows-msvc")+sizeof("xray:xir-runtime-abi-measurements:v1")+
        sizeof("xray:xir-runtime-recipe:windows-x86_64-hosted:v1")+sizeof("lib/test.lib")+sizeof("kernel32");
    CHECK(work==1+sizeof(current_sdk_kat_preimage)+encoded_integers+scanned_strings+2);
    /* Historical digests belong to their complete independently checked ABI
     * tables; the current identity encoder only emits the current table.
     * Prefix admission is allocation-free; the complete loader still owns its
     * finite manifest allocations and physical-file authentication. */
    const uint8_t *const previous_digests[]={previous66_sdk_kat_digest,previous67_sdk_kat_digest,previous68_sdk_kat_digest,previous69_sdk_kat_digest,previous70_sdk_kat_digest,previous71_sdk_kat_digest};
    manifest.prefix[3]=22;manifest.prefix[4]=28;manifest.prefix[5]=29;
    for (uint32_t i=0;i<6;++i) {
        manifest.prefix[1]=i==5?26u:25u;manifest.prefix[2]=66+i;json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
        size_t attempts=runtime_attempts;XrCompileResourceStats before=sdk_stats(resources);
        memcpy(manifest.digest,previous_digests[i],32);
        for (uint32_t wire=25;wire<=27;++wire) {
            manifest.prefix[1]=wire;json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
            XrXirSdkManifest preserved=manifest;
            CHECK(!sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_INVALID);
            CHECK(!memcmp(&manifest,&preserved,sizeof(manifest)) && runtime_attempts==attempts);
            XrCompileResourceStats after=sdk_stats(resources);
            CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
                after.live_bytes==before.live_bytes);
        }
    }
    manifest.prefix[1]=27;manifest.prefix[2]=72;
    json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    memcpy(manifest.digest,previous72_sdk_kat_digest,32);
    XrXirSdkManifest saved72=manifest;size_t attempts72=runtime_attempts;
    CHECK(!sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_INVALID);
    CHECK(!memcmp(&manifest,&saved72,sizeof(manifest)) && runtime_attempts==attempts72);
    memcpy(manifest.prefix,prefix,sizeof(prefix));
    manifest.prefix[3]=23;json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    memcpy(manifest.digest,previous_value23_sdk_kat_digest,32);
    XrXirSdkManifest saved23=manifest;size_t attempts23=runtime_attempts;
    CHECK(!sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_INVALID);
    CHECK(!memcmp(&manifest,&saved23,sizeof(manifest)) && runtime_attempts==attempts23);
    memcpy(manifest.prefix,prefix,sizeof(prefix));
    const uint32_t retired24_prefix[]={2,28,73,24,29,30,1,1,1,11,2,0,1,0,3,1,1};
    memcpy(manifest.prefix,retired24_prefix,sizeof(retired24_prefix));
    memcpy(manifest.digest,previous_value24_sdk_kat_digest,32);json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    XrXirSdkManifest saved24=manifest;size_t attempts24=runtime_attempts;
    XrCompileResourceStats before24=sdk_stats(resources);
    CHECK(!sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_INVALID);
    CHECK(!memcmp(&manifest,&saved24,sizeof(manifest)) && runtime_attempts==attempts24);
    XrCompileResourceStats after24=sdk_stats(resources);
    CHECK(after24.allocation_count==before24.allocation_count && after24.allocated_bytes==before24.allocated_bytes && after24.live_bytes==before24.live_bytes);
    uint8_t hash24[32];xr_sha256(previous_value24_sdk_kat_preimage,sizeof(previous_value24_sdk_kat_preimage),hash24);
    CHECK(!memcmp(hash24,previous_value24_sdk_kat_digest,32) && sizeof(previous_value24_sdk_kat_preimage)==3186);
    memcpy(manifest.prefix,prefix,sizeof(prefix));
    sdk_call32_prefix(resources,&manifest);
    sdk_semantic75_prefix(resources,&manifest);
    manifest.prefix[5]=34;memcpy(manifest.digest,previous_program34_sdk_kat_digest,32);
    json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    XrXirSdkManifest saved34=manifest;size_t attempts34=runtime_attempts;
    CHECK(!sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_INVALID);
    CHECK(!memcmp(&manifest,&saved34,sizeof(manifest)) && runtime_attempts==attempts34);
    uint8_t hash34[32];xr_sha256(previous_program34_sdk_kat_preimage,sizeof(previous_program34_sdk_kat_preimage),hash34);
    CHECK(sizeof(previous_program34_sdk_kat_preimage)==3194 && !memcmp(hash34,previous_program34_sdk_kat_digest,32));
    memcpy(manifest.prefix,prefix,sizeof(prefix));
    sdk_combined_previous_prefix(resources,&manifest,prefix);
    const uint32_t crossed[][2]={{1,25},{1,26},{1,27},{2,70},{2,71},{2,72},{2,73},{2,74},
        {3,22},{3,23},{3,24},{3,25},{3,26},{3,27},{3,28},{4,28},{4,29},{4,30},{4,31},{4,32},{5,29},{5,30},{5,31},{5,32},{5,33},{5,34}};
    for (uint32_t prior=0;prior<sizeof(crossed)/sizeof(crossed[0]);++prior) {
        uint32_t field=crossed[prior][0];manifest.prefix[field]=crossed[prior][1];
        json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
        size_t attempts=runtime_attempts;XrCompileResourceStats before=sdk_stats(resources);
        XrXirSdkManifest preserved=manifest;
        CHECK(!sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_INVALID);
        CHECK(!memcmp(&manifest,&preserved,sizeof(manifest)) && runtime_attempts==attempts);
        XrCompileResourceStats after=sdk_stats(resources);
        CHECK(after.allocation_count==before.allocation_count && after.allocated_bytes==before.allocated_bytes &&
            after.live_bytes==before.live_bytes);
        manifest.prefix[field]=prefix[field];
    }
    json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
    CHECK(sdk_json_prefix(&json,&manifest) && json.status==XR_JSON_CURSOR_OK);
    xr_compile_resources_release(resources);
    uint8_t hash[32];xr_sha256(current_sdk_kat_preimage,sizeof(current_sdk_kat_preimage),hash);
    CHECK(!memcmp(hash,current_sdk_kat_digest,32));
    for (uint32_t minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits={sdk_measurement_limits.allocated_bytes,sdk_measurement_limits.live_bytes,work-minus};
        resources=sdk_ledger(&limits);json=xr_json_cursor_make(NULL,0,resources,sdk_cursor_charge);
        memset(manifest.digest,0,32);
        if (minus) {
            CHECK(!sdk_identity(&json,&manifest) && json.status==XR_JSON_CURSOR_BUDGET);
            for (size_t i=0;i<32;++i) CHECK(!manifest.digest[i]);
        } else CHECK(sdk_identity(&json,&manifest) && !memcmp(manifest.digest,current_sdk_kat_digest,32));
        CHECK(sdk_stats(resources).work<=limits.work);
        xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
    }
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
    CHECK(facts.metadata_bytes==runtime_bytes-bytes && facts.work_used>facts.file_bytes*2);
    CHECK(used.live_bytes==runtime_bytes && facts.work_used==used.work-before.work);
    CHECK(!memcmp(facts.identity,sdk_expected_identity,32));sdk_locked_files(sdk);
    xr_xir_runtime_sdk_free(sdk);
    CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    for (size_t at=0;at<sites;++at) {
        runtime_attempts=0;runtime_fail_at=at;sdk=NULL;
        XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(original,&sdk);runtime_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_SDK_OUT_OF_MEMORY && !sdk && runtime_attempts>at);
        CHECK(runtime_live==live && runtime_bytes==bytes && sdk_handles()==handles);
    }
    for (size_t pass=0;pass<4;++pass) {
        XrXirRuntimeSdkRequest request=*original;
        XrCompileResourceLimits exact={sizeof(XrCompileResources)+facts.metadata_bytes-(pass==1),
            sizeof(XrCompileResources)+facts.metadata_bytes-(pass==2),1+facts.work_used-(pass==3)};
        request.resources=sdk_ledger(&exact);sdk=NULL;
        XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);
        CHECK(status==(pass ? XR_XIR_SDK_BUDGET : XR_XIR_SDK_OK));
        CHECK(pass ? !sdk : sdk!=NULL);xr_xir_runtime_sdk_free(sdk);
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
    printf("SDK actual allocation sites=%zu IO/OOM sites=%zu exact/minus1 allocated/live/work refunds PASS\n",sites,io_sites);
}
#include "xir_sdk_shared_resources.inc.c"
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--known-bytes")) { sdk_known_bytes(); return 0; }
    if ((argc!=3 && argc!=4) || (argc==4 && strcmp(argv[3],"status") && strcmp(argv[3],"--shared-resources"))) {
        fputs("usage: sdk-test ROOT MANIFEST [status|--shared-resources]\n",stderr);return 2;
    }
    bool shared_only=argc==4 && !strcmp(argv[3],"--shared-resources");
    if (!shared_only) sdk_known_bytes();
    size_t length=0;char *manifest=sdk_input(argv[2],&length);
    XrCompileResources *resources=sdk_ledger(&sdk_measurement_limits);
    XrXirRuntimeSdkRequest request={argv[1],manifest,length,resources};
    DWORD initial_handles=sdk_handles();
    if (shared_only) {
        sdk_shared_resources(&request,false);
        xr_free(manifest);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes && sdk_handles()==initial_handles);
        puts("SDK/Target shared-resources mode PASS; exhaustive SDK faults require the default mode");return 0;
    }
    if (argc==4) {
        XrXirRuntimeSdk *sdk=NULL;XrXirRuntimeSdkStatus status=xr_xir_runtime_sdk_load(&request,&sdk);
        printf("%u",(unsigned)status);
        if (sdk) {
            const XrXirRuntimeSdkFacts *facts=xr_xir_runtime_sdk_facts(sdk);
            printf(" %u ",facts->file_count);
            for (size_t i=0;i<32;++i) printf("%02x",facts->identity[i]);
        }
        puts("");xr_xir_runtime_sdk_free(sdk);xr_free(manifest);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes && sdk_handles()==initial_handles);return 0;
    }
    sdk_shared_resources(&request,true);sdk_fixed_work();sdk_bounded_scanning();
    XrXirRuntimeSdkRequest too_large=request;too_large.manifest_length=XR_XIR_SDK_MANIFEST_LIMIT+1;
    XrXirRuntimeSdk *empty=NULL;CHECK(xr_xir_runtime_sdk_load(&too_large,&empty)==XR_XIR_SDK_BUDGET && !empty);
    XrXirRuntimeSdkRequest no_resources=request;no_resources.resources=NULL;
    CHECK(xr_xir_runtime_sdk_load(&no_resources,&empty)==XR_XIR_SDK_INVALID && !empty);
    XrXirRuntimeSdk *unchanged=(XrXirRuntimeSdk *)(uintptr_t)1;size_t before=runtime_attempts;
    CHECK(xr_xir_runtime_sdk_load(&request,&unchanged)==XR_XIR_SDK_INVALID &&
        unchanged==(XrXirRuntimeSdk *)(uintptr_t)1 && runtime_attempts==before);
    char *producer_root=xr_malloc(strlen(request.root)+1);CHECK(producer_root);
    memcpy(producer_root,request.root,strlen(request.root)+1);request.root=producer_root;
    XrXirRuntimeSdk *sdk=NULL;CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
    const char *existing=(const char *)(uintptr_t)1;uint64_t query_work=sdk_stats(resources).work;
    CHECK(xr_xir_runtime_sdk_file(sdk,"ignored",&existing)==XR_XIR_SDK_INVALID &&
        existing==(const char *)(uintptr_t)1 && sdk_stats(resources).work==query_work);
    xr_free(manifest);xr_free(producer_root);xr_compile_resources_release(resources);
    CHECK(xr_xir_runtime_sdk_root(sdk) && xr_xir_runtime_sdk_facts(sdk)->value_abi==29);
    sdk_locked_files(sdk);xr_xir_runtime_sdk_free(sdk);CHECK(!runtime_live && !runtime_bytes && sdk_handles()==initial_handles);
    puts("SDK producer destroyed, immutable same-source facts and locked actual bundle PASS");return 0;
}
