/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_lease_owner.c - Native input ownership and same-handle read failures
 */
#include "app/toolchain/xtc_xir_file_lease.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s error=%lu\n",__LINE__,#c,GetLastError()); exit(1); } } while (0)
#define xr_compile_resources_work lease_real_work
#define xr_compile_resources_calloc lease_real_calloc
#define xr_compile_resources_alloc lease_real_alloc
#include "../../xir/xir_sdk_resource_test.h"
#undef xr_compile_resources_work
#undef xr_compile_resources_calloc
#undef xr_compile_resources_alloc
static bool record_work;
static uint64_t boundaries[32768];static size_t boundary_count;
static XrCompileResourceStatus remember_work(XrCompileResources *r,XrCompileResourceStatus status) {
    if(record_work&&status==XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats stats=sdk_stats(r);
        if(!boundary_count||boundaries[boundary_count-1]!=stats.work) {
            CHECK(boundary_count<32768);boundaries[boundary_count++]=stats.work;
        }
    }
    return status;
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r,uint64_t work) {
    return remember_work(r,lease_real_work(r,work));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(XrCompileResources *r,size_t count,size_t bytes,void **out) {
    return remember_work(r,lease_real_calloc(r,count,bytes,out));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(XrCompileResources *r,size_t bytes,void **out) {
    return remember_work(r,lease_real_alloc(r,bytes,out));
}
#ifndef LEASE_PRODUCTION
static size_t io_attempts,io_fail_at=SIZE_MAX;
static DWORD io_error=ERROR_READ_FAULT;
static bool short_read;
static bool fail_io(void) {
    if (io_attempts++!=io_fail_at)return false;
    SetLastError(io_error);return true;
}
static BOOL lease_read(HANDLE file,LPVOID data,DWORD bytes,LPDWORD count,LPOVERLAPPED over) {
    if(fail_io())return FALSE;
    if(short_read){*count=0;return TRUE;}
    return ReadFile(file,data,bytes,count,over);
}
#define CreateFileW(...) (fail_io()?INVALID_HANDLE_VALUE:CreateFileW(__VA_ARGS__))
#define ReadFile lease_read
#define GetFileInformationByHandle(...) (fail_io()?FALSE:GetFileInformationByHandle(__VA_ARGS__))
#define GetFileInformationByHandleEx(...) (fail_io()?FALSE:GetFileInformationByHandleEx(__VA_ARGS__))
#define GetFinalPathNameByHandleW(...) (fail_io()?0u:GetFinalPathNameByHandleW(__VA_ARGS__))
#define GetFileSizeEx(...) (fail_io()?FALSE:GetFileSizeEx(__VA_ARGS__))
#define GetFileType(...) (fail_io()?FILE_TYPE_UNKNOWN:GetFileType(__VA_ARGS__))
#define MultiByteToWideChar(...) (fail_io()?0:MultiByteToWideChar(__VA_ARGS__))
#define WideCharToMultiByte(...) (fail_io()?0:WideCharToMultiByte(__VA_ARGS__))
#define CompareStringOrdinal(...) (fail_io()?0:CompareStringOrdinal(__VA_ARGS__))
#define SetFilePointerEx(...) (fail_io()?FALSE:SetFilePointerEx(__VA_ARGS__))
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
#undef CompareStringOrdinal
#undef SetFilePointerEx
#endif
static char input_path[4096],empty_path[4096],child_path[4096];
static const char expected[]="same handle\0\xE4\xB8\xAD\xE6\x96\x87\r\n";
static DWORD handles(void) {DWORD n=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&n));return n;}
static void empty(void) {CHECK(!runtime_live&&!runtime_bytes);}
static XrXirTargetStatus transaction(const XrCompileResourceLimits *limits,bool publish,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XtcXirFileLease *file=NULL,*directory=NULL;
    void *first=NULL,*second=NULL;size_t first_length=0,second_length=0;
    memset(stats,0,sizeof(*stats));
    XrCompileResourceStatus made=xr_compile_resources_new(limits,&resources);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_TARGET_BUDGET:XR_XIR_TARGET_OUT_OF_MEMORY;
    XrXirTargetStatus status=xtc_xir_file_lease_directory_open(resources,child_path,&directory);
    if(status!=XR_XIR_TARGET_OK)goto done;
    status=xtc_xir_file_lease_open(resources,input_path,&file);if(status!=XR_XIR_TARGET_OK)goto done;
    xr_compile_resources_release(resources);
    CHECK(xtc_xir_file_lease_resources(file)==resources&&xtc_xir_file_lease_resources(directory)==resources);
    CHECK(!xtc_xir_file_lease_facts(directory)&&!xtc_xir_file_lease_directory_facts(file));
    const XtcXirDirectoryFacts *dir=xtc_xir_file_lease_directory_facts(directory);
    CHECK(dir&&dir->path&&dir->native_path&&dir->volume);
    status=xtc_xir_file_lease_read(file,4096,&first,&first_length);
    if(status==XR_XIR_TARGET_OK)status=xtc_xir_file_lease_read(file,4096,&second,&second_length);
    if(status==XR_XIR_TARGET_OK) {
        CHECK(first_length==sizeof(expected)-1&&second_length==first_length);
        CHECK(!memcmp(first,expected,first_length)&&!memcmp(first,second,first_length));
        const XtcXirFileFacts *facts=xtc_xir_file_lease_facts(file);
        CHECK(facts&&facts->length==first_length);
        if(publish){printf("BYTES %llu\nSHA ",(unsigned long long)facts->length);for(size_t i=0;i<32;++i)printf("%02x",facts->digest[i]);puts("");}
    }
    *stats=sdk_stats(resources);
    xtc_xir_file_lease_free(file);file=NULL;xtc_xir_file_lease_free(directory);directory=NULL;
    if(status==XR_XIR_TARGET_OK)CHECK(!memcmp(first,expected,first_length));
    xr_compile_resources_free(first);xr_compile_resources_free(second);empty();return status;
done:
    *stats=sdk_stats(resources);
    xtc_xir_file_lease_free(file);xtc_xir_file_lease_free(directory);xr_compile_resources_release(resources);empty();return status;
}
static void ownership(const char *root) {
    XrCompileResources *r=sdk_ledger(&sdk_unlimited);XtcXirFileLease *file=NULL,*directory=NULL;
    char *source=xr_malloc(strlen(input_path)+1);CHECK(source);strcpy(source,input_path);
    CHECK(xtc_xir_file_lease_open(r,source,&file)==XR_XIR_TARGET_OK);memset(source,'?',strlen(source));xr_free(source);
    CHECK(xtc_xir_file_lease_directory_open(r,root,&directory)==XR_XIR_TARGET_OK);
    XtcXirFileLease *duplicate=NULL;CHECK(xtc_xir_file_lease_open(r,input_path,&duplicate)==XR_XIR_TARGET_OK);
    void *duplicate_bytes=NULL;size_t duplicate_length=0;
    CHECK(xtc_xir_file_lease_read(duplicate,4096,&duplicate_bytes,&duplicate_length)==XR_XIR_TARGET_OK);
    CHECK(duplicate_length==sizeof(expected)-1&&!memcmp(duplicate_bytes,expected,duplicate_length));
    xtc_xir_file_lease_free(duplicate);xr_compile_resources_free(duplicate_bytes);
    wchar_t path[4096],child[4096];CHECK(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input_path,-1,path,4096));
    HANDLE denied=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);CHECK(denied==INVALID_HANDLE_VALUE);
    CHECK(!DeleteFileW(path));
    const XtcXirDirectoryFacts *dir=xtc_xir_file_lease_directory_facts(directory);
    CHECK(swprintf_s(child,4096,L"%ls\\new-child.bin",dir->native_path)>0);
    HANDLE created=CreateFileW(child,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,0,NULL);CHECK(created!=INVALID_HANDLE_VALUE);CHECK(CloseHandle(created)&&DeleteFileW(child));
    CHECK(swprintf_s(child,4096,L"%ls-renamed",dir->native_path)>0);CHECK(!MoveFileW(dir->native_path,child));
    size_t length=0;void *bytes=NULL;
    CHECK(xtc_xir_file_lease_read(directory,4096,&bytes,&length)==XR_XIR_TARGET_INVALID&&!bytes&&!length);
    void *sentinel=(void *)(uintptr_t)0x1234;size_t sentinel_length=9;
    CHECK(xtc_xir_file_lease_read(file,4096,&sentinel,&sentinel_length)==XR_XIR_TARGET_INVALID&&sentinel==(void *)(uintptr_t)0x1234&&sentinel_length==9);
    CHECK(xtc_xir_file_lease_read(file,sizeof(expected)-2,&bytes,&length)==XR_XIR_TARGET_BUDGET&&!bytes&&!length);
    CHECK(xtc_xir_file_lease_read(file,4096,&bytes,&length)==XR_XIR_TARGET_BUDGET&&!bytes&&!length);
    xtc_xir_file_lease_free(file);file=NULL;
    CHECK(xtc_xir_file_lease_open(r,empty_path,&file)==XR_XIR_TARGET_OK);
    CHECK(xtc_xir_file_lease_read(file,1,&bytes,&length)==XR_XIR_TARGET_OK&&bytes&&!length);
    xr_compile_resources_release(r);xtc_xir_file_lease_free(file);xtc_xir_file_lease_free(directory);xr_compile_resources_free(bytes);empty();
}
static void invalid_inputs(const char *root) {
    XrCompileResources *r=sdk_ledger(&sdk_unlimited);XtcXirFileLease *file=NULL;
    XtcXirFileLease *sentinel=(XtcXirFileLease *)(uintptr_t)0x1234;
    size_t before=runtime_attempts;
    CHECK(xtc_xir_file_lease_open(r,input_path,&sentinel)==XR_XIR_TARGET_INVALID&&sentinel==(XtcXirFileLease *)(uintptr_t)0x1234);
    CHECK(runtime_attempts==before);
    CHECK(xtc_xir_file_lease_open(r,"relative.bin",&file)==XR_XIR_TARGET_UNSUPPORTED&&!file);
    CHECK(xtc_xir_file_lease_open(r,child_path,&file)==XR_XIR_TARGET_INVALID&&!file);
    CHECK(xtc_xir_file_lease_directory_open(r,input_path,&file)==XR_XIR_TARGET_INVALID&&!file);
    char path[4096];CHECK(snprintf(path,sizeof(path),"%s/not-present",root)>0);
    CHECK(xtc_xir_file_lease_open(r,path,&file)==XR_XIR_TARGET_UNRESOLVED&&!file);
    CHECK(snprintf(path,sizeof(path),"%s:stream",input_path)>0);
    CHECK(xtc_xir_file_lease_open(r,path,&file)==XR_XIR_TARGET_INVALID&&!file);
    CHECK(snprintf(path,sizeof(path),"%s/.. /input.bin",root)>0);
    CHECK(xtc_xir_file_lease_open(r,path,&file)==XR_XIR_TARGET_INVALID&&!file);
    CHECK(snprintf(path,sizeof(path),"%s/\xE4\xB8\xAD\xE6\x96\x87.bin",root)>0);
    CHECK(xtc_xir_file_lease_open(r,path,&file)==XR_XIR_TARGET_OK);
    void *bytes=NULL;size_t length=0;CHECK(xtc_xir_file_lease_read(file,4096,&bytes,&length)==XR_XIR_TARGET_OK);
    CHECK(length==sizeof(expected)-1&&!memcmp(bytes,expected,length));xr_compile_resources_free(bytes);xtc_xir_file_lease_free(file);
    file=NULL;char drive[]={root[0],':','/',0};
    CHECK(xtc_xir_file_lease_directory_open(r,drive,&file)==XR_XIR_TARGET_OK);
    CHECK(strlen(xtc_xir_file_lease_directory_facts(file)->path)==3);xtc_xir_file_lease_free(file);
    xr_compile_resources_release(r);empty();
}
#ifndef LEASE_PRODUCTION
static void short_owned_read(void) {
    XrCompileResources *r=sdk_ledger(&sdk_unlimited);XtcXirFileLease *file=NULL;
    CHECK(xtc_xir_file_lease_open(r,input_path,&file)==XR_XIR_TARGET_OK);
    void *bytes=NULL;size_t length=0;short_read=true;
    CHECK(xtc_xir_file_lease_read(file,4096,&bytes,&length)==XR_XIR_TARGET_IO&&!bytes&&!length);
    short_read=false;size_t previous=io_attempts;
    CHECK(xtc_xir_file_lease_read(file,4096,&bytes,&length)==XR_XIR_TARGET_IO&&!bytes&&!length&&io_attempts==previous);
    CHECK(xtc_xir_file_lease_facts(file)->length==sizeof(expected)-1);
    xtc_xir_file_lease_free(file);xr_compile_resources_release(r);empty();
}
#endif
int main(int argc,char **argv) {
    CHECK(argc==2);CHECK(snprintf(input_path,sizeof(input_path),"%s/input.bin",argv[1])>0);
    CHECK(snprintf(empty_path,sizeof(empty_path),"%s/empty.bin",argv[1])>0);
    CHECK(snprintf(child_path,sizeof(child_path),"%s/child",argv[1])>0);
    XrCompileResourceStats baseline,current;
    CHECK(transaction(&sdk_unlimited,false,&current)==XR_XIR_TARGET_OK);ownership(argv[1]);invalid_inputs(argv[1]);
    DWORD before=handles();runtime_attempts=0;
#ifndef LEASE_PRODUCTION
    io_attempts=0;
#endif
    record_work=true;CHECK(transaction(&sdk_unlimited,true,&baseline)==XR_XIR_TARGET_OK);record_work=false;size_t count=runtime_attempts;
#ifndef LEASE_PRODUCTION
    size_t io_count=io_attempts;
#endif
    for(size_t i=0;i<count;++i){runtime_attempts=0;runtime_fail_at=i;CHECK(transaction(&sdk_unlimited,false,&current)==XR_XIR_TARGET_OUT_OF_MEMORY);empty();CHECK(handles()==before);}
    runtime_fail_at=SIZE_MAX;
    for(size_t i=0;i<boundary_count;++i) {
        XrCompileResourceLimits limits=sdk_unlimited;limits.work=boundaries[i]-1;
        CHECK(transaction(&limits,false,&current)==XR_XIR_TARGET_BUDGET);empty();CHECK(handles()==before);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned below=0;below<2;++below) {
        XrCompileResourceLimits limits=sdk_unlimited;
        if(axis==0)limits.allocated_bytes=baseline.allocated_bytes-below;
        if(axis==1)limits.live_bytes=baseline.peak_bytes-below;
        if(axis==2)limits.work=baseline.work-below;
        CHECK(transaction(&limits,false,&current)==(below?XR_XIR_TARGET_BUDGET:XR_XIR_TARGET_OK));empty();CHECK(handles()==before);
    }
#ifndef LEASE_PRODUCTION
    DWORD errors[]={ERROR_READ_FAULT,ERROR_OUTOFMEMORY,ERROR_NOT_ENOUGH_MEMORY};
    for(size_t e=0;e<3;++e)for(size_t i=0;i<io_count;++i){io_attempts=0;io_fail_at=i;io_error=errors[e];CHECK(transaction(&sdk_unlimited,false,&current)==(e?XR_XIR_TARGET_OUT_OF_MEMORY:XR_XIR_TARGET_IO));empty();CHECK(handles()==before);}
    io_fail_at=SIZE_MAX;short_read=true;CHECK(transaction(&sdk_unlimited,false,&current)==XR_XIR_TARGET_IO);short_read=false;
    short_owned_read();
    printf("IO %zu points x 3 statuses\n",io_count);
#endif
    CHECK(handles()==before);printf("OOM %zu allocations; work %zu boundaries; three axes exact/minus1; owned physical heap/handles zero\n",count,boundary_count);return 0;
}
