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
    XrXirTargetCommandFacts command={"C:/described-tool.exe",sdk->root,argv,1,NULL,0,3000,1048576,0,0};
    XrXirTargetDependency file={path,XR_XIR_TARGET_HEADER};
    XrXirTargetSnapshotRequest request={sdk->resources,"x86_64-windows-msvc",3,2,11,&file,1,&command,1,NULL};
    return xtc_xir_target_snapshot_capture(&request,output);
}
static void sdk_target_faults(const XrXirRuntimeSdkRequest *request) {
    XrXirRuntimeSdk *sdk=NULL;CHECK(xr_xir_runtime_sdk_load(request,&sdk)==XR_XIR_SDK_OK);
    uint64_t baseline=sdk_stats(request->resources).held_bytes;DWORD handles=sdk_handles();
    size_t physical=runtime_bytes;runtime_attempts=0;sdk_io_attempts=0;
    XrXirTargetSnapshot *target=NULL;CHECK(sdk_target(request,&target)==XR_XIR_TARGET_OK);
    size_t allocations=runtime_attempts,ios=sdk_io_attempts;xtc_xir_target_free(target);
    for (size_t i=0;i<allocations;++i) {
        runtime_attempts=0;runtime_fail_at=i;target=NULL;
        CHECK(sdk_target(request,&target)==XR_XIR_TARGET_OUT_OF_MEMORY && !target);runtime_fail_at=SIZE_MAX;
        CHECK(sdk_stats(request->resources).held_bytes==baseline && runtime_bytes==physical && sdk_handles()==handles);
    }
    for (size_t i=0;i<ios;++i) for (unsigned mode=0;mode<2;++mode) {
        sdk_io_attempts=0;sdk_io_fail_at=i;sdk_io_error=mode ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT;target=NULL;
        CHECK(sdk_target(request,&target)==(mode ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_IO) && !target);
        sdk_io_fail_at=SIZE_MAX;
        CHECK(sdk_stats(request->resources).held_bytes==baseline && runtime_bytes==physical && sdk_handles()==handles);
    }
    sdk_io_error=ERROR_READ_FAULT;CHECK(xr_xir_runtime_sdk_facts(sdk)->value_abi==30);
    xr_xir_runtime_sdk_free(sdk);
    printf("Target with SDK alive: %zu allocator and %zu IO/OOM points PASS\n",allocations,ios);
}
static void sdk_faults_with_target(const XrXirRuntimeSdkRequest *original) {
    XrXirTargetSnapshot *target=NULL;CHECK(sdk_target(original,&target)==XR_XIR_TARGET_OK);
    sdk_actual_faults(original);CHECK(xtc_xir_target_file(target,0)->length>0);xtc_xir_target_free(target);
}
static void sdk_shared_resources(const XrXirRuntimeSdkRequest *original,bool run_sdk_faults) {
    size_t physical=runtime_bytes;DWORD handles=sdk_handles();
    XrXirRuntimeSdkRequest request=*original;request.resources=sdk_ledger(&sdk_measurement_limits);
    XrXirRuntimeSdk *sdk=NULL;XrXirTargetSnapshot *target=NULL;
    CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
    uint64_t sdk_live=sdk_stats(request.resources).held_bytes;
    CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_OK);
    XrCompileResourceStats both=sdk_stats(request.resources);
    CHECK(both.held_bytes==runtime_bytes-physical && both.peak_bytes==both.held_bytes && both.held_bytes>sdk_live);
    xtc_xir_target_free(target);xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(request.resources);
    XrCompileResourceLimits exact={XR_COMPILE_MEMORY_FINITE,both.peak_bytes,true};
    for (unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=exact;limits.memory_limit-=minus;
        request.resources=sdk_ledger(&limits);sdk=NULL;target=NULL;
        CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
        uint64_t baseline=sdk_stats(request.resources).held_bytes;
        CHECK(sdk_target(&request,&target)==(minus ? XR_XIR_TARGET_MEMORY_LIMIT : XR_XIR_TARGET_OK));
        if (minus) {
            CHECK(!target && sdk_stats(request.resources).held_bytes==baseline);
            CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_MEMORY_LIMIT && !target);
            CHECK(sdk_stats(request.resources).held_bytes==baseline);
        }
        CHECK(xr_xir_runtime_sdk_facts(sdk)->file_count>0);
        xr_xir_runtime_sdk_free(sdk);xtc_xir_target_free(target);xr_compile_resources_release(request.resources);
        CHECK(runtime_bytes==physical && sdk_handles()==handles);
    }
    for (unsigned reverse=0;reverse<2;++reverse) {
        request.resources=sdk_ledger(&sdk_measurement_limits);sdk=NULL;target=NULL;
        CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_OK);
        CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
        xr_compile_resources_release(request.resources);
        CHECK(xtc_xir_target_file(target,0)->length>0 && xr_xir_runtime_sdk_root(sdk));
        if (reverse) {xr_xir_runtime_sdk_free(sdk);CHECK(xtc_xir_target_facts(target)->file_count==1);xtc_xir_target_free(target);}
        else {xtc_xir_target_free(target);sdk_locked_files(sdk);xr_xir_runtime_sdk_free(sdk);}
        CHECK(runtime_bytes==physical && sdk_handles()==handles);
    }
    /* Releasing live allocations restores finite resident memory admission. */
    XrCompileResourceLimits once={XR_COMPILE_MEMORY_FINITE,both.peak_bytes,true};
    request.resources=sdk_ledger(&once);sdk=NULL;target=NULL;
    CHECK(sdk_target(&request,&target)==XR_XIR_TARGET_OK);
    CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK);
    xtc_xir_target_free(target);xr_xir_runtime_sdk_free(sdk);sdk=NULL;
    uint64_t turnover=sdk_stats(request.resources).allocated_bytes;
    CHECK(xr_xir_runtime_sdk_load(&request,&sdk)==XR_XIR_SDK_OK && sdk);
    CHECK(sdk_stats(request.resources).allocated_bytes>turnover);
    xr_xir_runtime_sdk_free(sdk);xr_compile_resources_release(request.resources);
    /* The exhaustive SDK matrix keeps its position before Target failures. */
    if (run_sdk_faults) sdk_faults_with_target(original);
    sdk_target_faults(original);
    CHECK(runtime_bytes==physical && sdk_handles()==handles);
    puts("SDK + Target resident-memory exact-minus1, reuse, producer release and both destruction orders PASS");
}
static void sdk_io_and_utf8(void) {
    XrCompileResources *resources=sdk_ledger(&sdk_measurement_limits);
    XrCompileResourceStats before=sdk_stats(resources);
    XrJsonCursor json=xr_json_cursor_make(NULL,0);size_t length=0;
    CHECK(xr_json_cursor_length(&json,"abc",&length) && length==3);
    CHECK(xr_json_cursor_equal(&json,"a","a"));
    char source[]="\"\\u0061\"";json.begin=json.cursor=source;json.end=source+8;
    const char *text=NULL;
    CHECK(xr_json_cursor_string(&json,1,&text) && !strcmp(text,"a"));
    XrCompileResourceStats after=sdk_stats(resources);CHECK(!memcmp(&before,&after,sizeof(before)));
    XrXirRuntimeSdk sdk={0};sdk.resources=resources;
    CHECK(!strcmp(sdk_utf8(&sdk,L"C:\\a",4),"C:/a"));
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
        CHECK(sdk_file_hash(&sdk,&file)==(mode==0));
        CHECK(sdk.status==(mode==0 ? XR_XIR_SDK_OK : mode==2 ? XR_XIR_SDK_OUT_OF_MEMORY : XR_XIR_SDK_IO));
    }
    sdk_io_fail_at=SIZE_MAX;sdk_io_error=ERROR_READ_FAULT;sdk_short_read=false;
    CHECK(CloseHandle(handle) && DeleteFileA(path));
    xr_compile_resources_release(resources);
    puts("SDK JSON grammar, UTF16 conversion and actual read IO/OOM/short-read before hash PASS");
}
static void sdk_bounded_scanning(void) {
    SYSTEM_INFO info;GetSystemInfo(&info);size_t page=info.dwPageSize;
    char *memory=VirtualAlloc(NULL,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);CHECK(memory);
    DWORD old;CHECK(VirtualProtect(memory+page,page,PAGE_NOACCESS,&old));
    memory[page-2]='x';memory[page-1]=0;
    XrJsonCursor json=xr_json_cursor_make(NULL,0);size_t length=99;
    CHECK(xr_json_cursor_length(&json,memory+page-2,&length) && length==1);
    memory[page-4]='"';memory[page-3]='x';memory[page-2]='x';memory[page-1]='"';
    const char *text=(const char *)(uintptr_t)1;
    json=xr_json_cursor_make(memory+page-4,4);
    CHECK(!xr_json_cursor_string(&json,1,&text) && text==(const char *)(uintptr_t)1 && json.status==XR_JSON_CURSOR_BUDGET);
    json=xr_json_cursor_make(memory+page-4,3);
    CHECK(!xr_json_cursor_string(&json,8,&text) && text==(const char *)(uintptr_t)1 && json.status==XR_JSON_CURSOR_INVALID);
    json.status=XR_JSON_CURSOR_BUDGET;json.cursor=memory+page;json.end=memory+page+1;
    CHECK(!xr_json_cursor_space(&json) && json.status==XR_JSON_CURSOR_BUDGET);
    CHECK(!xr_json_cursor_equal(&json,memory+page,memory+page) && json.status==XR_JSON_CURSOR_BUDGET);
    CHECK(VirtualFree(memory,0,MEM_RELEASE));
}
