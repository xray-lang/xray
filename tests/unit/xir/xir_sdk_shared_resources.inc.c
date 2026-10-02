/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_sdk_shared_resources.inc.c - SDK and input snapshots share actual resources
 */
static XrXirTargetStatus sdk_target(const XrXirRuntimeSdkRequest *sdk,XrXirTargetSnapshot **output) {
    char path[32768];CHECK(snprintf(path,sizeof(path),"%s/src/base/xdefs.h",sdk->root)>0);
    const char *argv[]={"diagnostic-input-snapshot"};
    XrXirTargetCommand command={sdk->root,argv,1,NULL,0};
    XrXirTargetDependency file={path,XR_XIR_TARGET_HEADER};
    XrXirTargetRequest request={sdk->resources,"x86_64-windows-msvc",3,2,11,&file,1,&command,1};
    return xtc_xir_target_capture(&request,output);
}
static void sdk_target_faults(const XrXirRuntimeSdkRequest *request) {
    XrXirRuntimeSdk *sdk=NULL;CHECK(xr_xir_runtime_sdk_load(request,&sdk)==XR_XIR_SDK_OK);
    uint64_t baseline=sdk_stats(request->resources).live_bytes;DWORD handles=sdk_handles();
    size_t physical=runtime_bytes;runtime_attempts=0;sdk_io_attempts=0;
    XrXirTargetSnapshot *target=NULL;CHECK(sdk_target(request,&target)==XR_XIR_TARGET_OK);
    size_t allocations=runtime_attempts,ios=sdk_io_attempts;xtc_xir_target_free(target);
    for (size_t i=0;i<allocations;++i) {
        runtime_attempts=0;runtime_fail_at=i;target=NULL;
        CHECK(sdk_target(request,&target)==XR_XIR_TARGET_OUT_OF_MEMORY && !target);runtime_fail_at=SIZE_MAX;
        CHECK(sdk_stats(request->resources).live_bytes==baseline && runtime_bytes==physical && sdk_handles()==handles);
    }
    for (size_t i=0;i<ios;++i) for (unsigned mode=0;mode<2;++mode) {
        sdk_io_attempts=0;sdk_io_fail_at=i;sdk_io_error=mode ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT;target=NULL;
        CHECK(sdk_target(request,&target)==(mode ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_IO) && !target);
        sdk_io_fail_at=SIZE_MAX;
        CHECK(sdk_stats(request->resources).live_bytes==baseline && runtime_bytes==physical && sdk_handles()==handles);
    }
    sdk_io_error=ERROR_READ_FAULT;CHECK(xr_xir_runtime_sdk_facts(sdk)->value_abi==17);
    xr_xir_runtime_sdk_free(sdk);
    printf("Target with SDK alive: %zu allocator and %zu IO/OOM points PASS\n",allocations,ios);
}
static void sdk_shared_resources(const XrXirRuntimeSdkRequest *original) {
    size_t physical=runtime_bytes;DWORD handles=sdk_handles();
    XrXirRuntimeSdkRequest request=*original;request.resources=sdk_ledger(&sdk_unlimited);
    XrXirRuntimeSdk *sdk=NULL;XrXirTargetSnapshot *target=NULL;
    CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
    uint64_t sdk_live=sdk_stats(request.resources).live_bytes;
    CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_OK);
    XrCompileResourceStats both=sdk_stats(request.resources);
    CHECK(both.live_bytes==runtime_bytes-physical && both.peak_bytes==both.live_bytes && both.live_bytes>sdk_live);
    xtc_xir_target_free(target);xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(request.resources);
    XrCompileResourceLimits exact={both.allocated_bytes,both.peak_bytes,both.work};
    for (unsigned pass=0;pass<4;++pass) {
        XrCompileResourceLimits limits=exact;
        if (pass==1) --limits.allocated_bytes;else if (pass==2) --limits.live_bytes;else if (pass==3) --limits.work;
        request.resources=sdk_ledger(&limits);sdk=NULL;target=NULL;
        CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
        uint64_t baseline=sdk_stats(request.resources).live_bytes;
        CHECK(sdk_target(&request,&target)==(pass ? XR_XIR_TARGET_BUDGET : XR_XIR_TARGET_OK));
        if (pass) {
            CHECK(!target && sdk_stats(request.resources).live_bytes==baseline);
            XrCompileResourceStats failed=sdk_stats(request.resources);
            CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_BUDGET && !target);
            XrCompileResourceStats retried=sdk_stats(request.resources);
            CHECK(retried.work>=failed.work && retried.allocated_bytes>=failed.allocated_bytes);
        }
        CHECK(xr_xir_runtime_sdk_facts(sdk)->file_count>0);
        xr_xir_runtime_sdk_free(sdk);xtc_xir_target_free(target);xr_compile_resources_release(request.resources);
        CHECK(runtime_bytes==physical && sdk_handles()==handles);
    }
    for (unsigned reverse=0;reverse<2;++reverse) {
        request.resources=sdk_ledger(&sdk_unlimited);sdk=NULL;target=NULL;
        CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_OK);
        CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
        xr_compile_resources_release(request.resources);
        CHECK(xtc_xir_target_file(target,0)->length>0 && xr_xir_runtime_sdk_root(sdk));
        if (reverse) {xr_xir_runtime_sdk_free(sdk);CHECK(xtc_xir_target_facts(target)->file_count==1);xtc_xir_target_free(target);}
        else {xtc_xir_target_free(target);sdk_locked_files(sdk);xr_xir_runtime_sdk_free(sdk);}
        CHECK(runtime_bytes==physical && sdk_handles()==handles);
    }
    /* Released live bytes do not replenish cumulative allocations for a retry. */
    XrCompileResourceLimits once={both.allocated_bytes,UINT64_MAX,UINT64_MAX};
    request.resources=sdk_ledger(&once);sdk=NULL;target=NULL;
    CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_OK);
    CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
    xtc_xir_target_free(target);xr_xir_runtime_sdk_free(sdk);sdk=NULL;
    uint64_t cumulative=sdk_stats(request.resources).allocated_bytes;
    CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_BUDGET && !sdk);
    CHECK(sdk_stats(request.resources).allocated_bytes==cumulative);
    xr_compile_resources_release(request.resources);
    /* Exercise SDK failures while an independent input owner holds the same ledger. */
    target=NULL;CHECK(sdk_target(original,&target)==XR_XIR_TARGET_OK);
    sdk_actual_faults(original);CHECK(xtc_xir_target_file(target,0)->length>0);xtc_xir_target_free(target);
    sdk_target_faults(original);
    CHECK(runtime_bytes==physical && sdk_handles()==handles);
    puts("SDK + Target cumulative/peak/work exact-minus1, no refresh, producer release and both destruction orders PASS");
}
static void sdk_fixed_work(void) {
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    SdkJson json={NULL,NULL,NULL,resources,XR_XIR_SDK_OK};size_t length=0;
    uint64_t before=sdk_stats(resources).work;
    CHECK(sdk_json_length(&json,"abc",&length) && length==3);
    CHECK(sdk_stats(resources).work-before==4);
    before=sdk_stats(resources).work;CHECK(sdk_json_equal(&json,"a","a"));
    CHECK(sdk_stats(resources).work-before==4);
    char source[]="\"\\u0061\"";json.begin=json.cursor=source;json.end=source+8;
    const char *text=NULL;before=sdk_stats(resources).work;
    CHECK(sdk_json_string(&json,1,&text) && !strcmp(text,"a"));
    CHECK(sdk_stats(resources).work-before==11); /* Eight reads, one peek, two writes. */
    XrXirRuntimeSdk sdk={0};sdk.resources=resources;
    before=sdk_stats(resources).work;CHECK(!strcmp(sdk_utf8(&sdk,L"C:\\a",4),"C:/a"));
    CHECK(sdk_stats(resources).work-before==2*4*sizeof(wchar_t)+1+sizeof(SdkMemory)+5+4);
    while (sdk.memory) {SdkMemory *next=sdk.memory->next;xr_compile_resources_free(sdk.memory);sdk.memory=next;}
    char directory[MAX_PATH],path[MAX_PATH];CHECK(GetTempPathA(MAX_PATH,directory));
    CHECK(GetTempFileNameA(directory,"xrs",0,path));
    FILE *stream=fopen(path,"wb");CHECK(stream && fwrite("abc",1,3,stream)==3 && !fclose(stream));
    HANDLE handle=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(handle!=INVALID_HANDLE_VALUE);wchar_t scratch[SDK_WIDE_CAPACITY];sdk.scratch=scratch;
    XrXirSdkFile file={0};file.handle=handle;file.length=3;xr_sha256((const uint8_t *)"abc",3,file.digest);
    for (unsigned mode=0;mode<4;++mode) {
        LARGE_INTEGER zero={0};CHECK(SetFilePointerEx(handle,zero,NULL,FILE_BEGIN));
        sdk.status=XR_XIR_SDK_OK;sdk_io_attempts=0;sdk_io_fail_at=mode==1 || mode==2 ? 1 : SIZE_MAX;
        sdk_io_error=mode==2 ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT;sdk_short_read=mode==3;
        before=sdk_stats(resources).work;
        CHECK(sdk_file_hash(&sdk,&file)==(mode==0));
        CHECK(sdk_stats(resources).work-before==(mode==0 ? 3+2*3+32 : 2+3));
        CHECK(sdk.status==(mode==0 ? XR_XIR_SDK_OK : mode==2 ? XR_XIR_SDK_OUT_OF_MEMORY : XR_XIR_SDK_IO));
    }
    sdk_io_fail_at=SIZE_MAX;sdk_io_error=ERROR_READ_FAULT;sdk_short_read=false;
    CHECK(CloseHandle(handle) && DeleteFileA(path));
    xr_compile_resources_release(resources);
    puts("SDK fixed work: JSON scanning/decoded writes, UTF16 input bytes, read IO/OOM/short-read before hash PASS");
}
static void sdk_bounded_scanning(void) {
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    char *memory=VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(memory);
    DWORD old;CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old));memory[page-1]='x';
    XrCompileResourceLimits limit={UINT64_MAX,UINT64_MAX,2};
    XrCompileResources *resources=sdk_ledger(&limit);SdkJson json={NULL,NULL,NULL,resources,XR_XIR_SDK_OK};
    size_t length=99;CHECK(!sdk_json_length(&json,memory+page-1,&length) && length==99 && json.status==XR_XIR_SDK_BUDGET);
    CHECK(sdk_stats(resources).work==2);
    json.status=XR_XIR_SDK_OK;json.cursor=memory+page;json.end=memory+page+1;
    CHECK(!sdk_json_space(&json) && json.status==XR_XIR_SDK_BUDGET);
    XrXirRuntimeSdk sdk={0};sdk.resources=resources;
    sdk.manifest.file_count=1;sdk.manifest.files[0].path=memory+page;
    const char *path=NULL;
    CHECK(xr_xir_runtime_sdk_file(&sdk,memory+page,&path)==XR_XIR_SDK_BUDGET && !path);
    xr_compile_resources_release(resources);CHECK(VirtualFree(memory,0,MEM_RELEASE));
}
