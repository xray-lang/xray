/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_publication_owner.c - Actual output replacement and resource ownership
 */
#include "app/toolchain/xtc_xir_publication.h"
#include "app/toolchain/xtc_xir_file_lease.h"
#include "base/xwindows_utf8.h"
#include "os/os_fs.h"
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"line=%d %s error=%lu\n",__LINE__,#c,GetLastError());exit(90);} } while(0)
#if PUBLICATION_INJECTED
#include "publication_faults.inc.h"
#endif
static XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
static const char payload[]="complete\0bytes";
static void old_file(const char *path) {
    FILE *file=fopen(path,"wb");CHECK(file);CHECK(fwrite("OLD",1,3,file)==3);CHECK(!fclose(file));
}
static void file_is(const char *path,const void *bytes,size_t length) {
    FILE *file=fopen(path,"rb");CHECK(file);char actual[64];size_t count=fread(actual,1,sizeof(actual),file);
    CHECK(!ferror(file) && !fclose(file));CHECK(count==length && !memcmp(actual,bytes,length));
}
static XtcXirPublicationStatus replace(const char *path,bool commit,XrCompileResourceStats *stats) {
    XrCompileResources *resources=NULL;XtcXirPublication *owner=NULL;
    XrCompileResourceStatus opened=xr_compile_resources_new(&limits,&resources);
    if(opened!=XR_COMPILE_RESOURCE_OK)return opened==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XTC_XIR_PUBLICATION_OUT_OF_MEMORY:XTC_XIR_PUBLICATION_BUDGET;
    XrCompileResourceStats empty={0};CHECK(xr_compile_resources_stats(resources,&empty)==XR_COMPILE_RESOURCE_OK);
    XtcXirPublicationRequest request={path,{4096,8,4096}};
    XtcXirPublicationStatus status=xtc_xir_publication_new(resources,&request,&owner);
    if(status==XTC_XIR_PUBLICATION_OK)status=xtc_xir_publication_write(owner,payload,sizeof(payload)-1);
    if(status==XTC_XIR_PUBLICATION_OK && commit)status=xtc_xir_publication_commit(owner);
    CHECK(xr_compile_resources_stats(resources,stats)==XR_COMPILE_RESOURCE_OK);
    if(status!=XTC_XIR_PUBLICATION_OK && owner) {
        CHECK(xtc_xir_publication_write(owner,payload,sizeof(payload)-1)==status);
        CHECK(xtc_xir_publication_commit(owner)==status);
        XrCompileResourceStats sticky;CHECK(xr_compile_resources_stats(resources,&sticky)==XR_COMPILE_RESOURCE_OK);
        CHECK(!memcmp(&sticky,stats,sizeof(sticky)));
    }
    CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK && !owner);
    XrCompileResourceStats closed;CHECK(xr_compile_resources_stats(resources,&closed)==XR_COMPILE_RESOURCE_OK);
    CHECK(closed.live_bytes==empty.live_bytes && closed.work==stats->work);
    xr_compile_resources_release(resources);
#if PUBLICATION_INJECTED
    CHECK(!live_allocations);
#endif
    return status;
}
static XtcXirPublication *ready(const char *path,XrCompileResources **resources) {
    CHECK(xr_compile_resources_new(&limits,resources)==XR_COMPILE_RESOURCE_OK);
    XtcXirPublication *owner=NULL;XtcXirPublicationRequest request={path,{4096,8,4096}};
    CHECK(xtc_xir_publication_new(*resources,&request,&owner)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_write(owner,payload,sizeof(payload)-1)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_phase(owner)==XTC_XIR_PUBLICATION_READY);return owner;
}
static void lifetime_and_limits(const char *path) {
    XrCompileResources *resources=NULL;XtcXirPublication *owner=ready(path,&resources);
    xr_compile_resources_release(resources);resources=NULL;
    CHECK(xtc_xir_publication_commit(owner)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_diagnostic(owner)->published);
    CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK);file_is(path,payload,sizeof(payload)-1);
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XtcXirPublicationRequest request={path,{4096,8,3}};
    XtcXirPublication *sentinel=(XtcXirPublication *)(uintptr_t)1;
    CHECK(xtc_xir_publication_new(resources,&request,&sentinel)==XTC_XIR_PUBLICATION_INVALID);
    CHECK(sentinel==(XtcXirPublication *)(uintptr_t)1);
    CHECK(xtc_xir_publication_new(resources,&request,&owner)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_write(owner,"size",4)==XTC_XIR_PUBLICATION_BUDGET);
    CHECK(xtc_xir_publication_commit(owner)==XTC_XIR_PUBLICATION_BUDGET);
    CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK);file_is(path,payload,sizeof(payload)-1);
    CHECK(xtc_xir_publication_new(resources,&request,&owner)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_write(owner,NULL,0)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_commit(owner)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK);file_is(path,"",0);
    CHECK(xtc_xir_publication_new(resources,&request,&owner)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_write(owner,"abc",3)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_commit(owner)==XTC_XIR_PUBLICATION_OK);
    CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK);file_is(path,"abc",3);
    xr_compile_resources_release(resources);
}
static DWORD winapi_control(void) {
    DWORD cold=0,volume=0,random=0,again=0,last=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&cold));
    HANDLE dir=CreateFileW(L".",FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
        OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(dir!=INVALID_HANDLE_VALUE);
    wchar_t filesystem[16];CHECK(GetVolumeInformationByHandleW(dir,NULL,0,NULL,NULL,NULL,filesystem,16));CHECK(CloseHandle(dir));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&volume));
    UCHAR nonce[16];CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&random));
    CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&again));
    CHECK(BCryptGenRandom(NULL,nonce,sizeof(nonce),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&last));CHECK(again==last);
    fprintf(stderr,"raw WinAPI controls cold=%lu volume-closed=%lu BCrypt=%lu repeated=%lu/%lu\n",cold,volume,random,again,last);
    return last;
}
static void lease_sharing(const char *path) {
    char parent[4096],child[4096],moved_parent[4096],moved_child[4096],first[4096],second[4096];
    CHECK(snprintf(parent,sizeof(parent),"%s.lease",path)>0);
    CHECK(snprintf(child,sizeof(child),"%s/child",parent)>0);
    CHECK(snprintf(moved_parent,sizeof(moved_parent),"%s.moved",parent)>0);
    CHECK(snprintf(moved_child,sizeof(moved_child),"%s/moved",parent)>0);
    CHECK(snprintf(first,sizeof(first),"%s/first",child)>0);
    CHECK(snprintf(second,sizeof(second),"%s/second",child)>0);
    CHECK(CreateDirectoryA(parent,NULL) && CreateDirectoryA(child,NULL));
    XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XtcXirFileLease *directory=NULL,*file=NULL;
    CHECK(xtc_xir_file_lease_directory_open(resources,child,&directory)==XR_XIR_TARGET_OK);
    CHECK(!MoveFileExA(child,moved_child,0) && GetLastError()==ERROR_SHARING_VIOLATION);
    CHECK(!MoveFileExA(parent,moved_parent,0) && GetLastError()==ERROR_SHARING_VIOLATION);
    old_file(first);old_file(second);CHECK(MoveFileExA(first,second,MOVEFILE_REPLACE_EXISTING));
    CHECK(xtc_xir_file_lease_open(resources,second,&file)==XR_XIR_TARGET_OK);
    HANDLE write=CreateFileA(second,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(write==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    CHECK(!DeleteFileA(second) && GetLastError()==ERROR_SHARING_VIOLATION);
    xtc_xir_file_lease_free(file);xtc_xir_file_lease_free(directory);xr_compile_resources_release(resources);
    CHECK(DeleteFileA(second) && RemoveDirectoryA(child) && RemoveDirectoryA(parent));
}
static void operation_work(const char *path) {
    XrCompileResourceStats writes[2];
    for(unsigned i=0;i<2;++i) {
        XrCompileResources *resources=NULL;CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XtcXirPublicationRequest request={path,{4096,8,4096}};XtcXirPublication *owner=NULL;
        CHECK(xtc_xir_publication_new(resources,&request,&owner)==XTC_XIR_PUBLICATION_OK);
        CHECK(xtc_xir_publication_write(owner,payload,i?sizeof(payload)-1:0)==XTC_XIR_PUBLICATION_OK);
        CHECK(xr_compile_resources_stats(resources,&writes[i])==XR_COMPILE_RESOURCE_OK);
        if(i) {
            wchar_t absolute[4096];int units=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,absolute,4096);
            CHECK(units>0 && absolute[1]==L':');
            size_t expected_rename=offsetof(FILE_RENAME_INFO,FileName)+((size_t)units+4)*sizeof(wchar_t);
            CHECK(xtc_xir_publication_commit(owner)==XTC_XIR_PUBLICATION_OK);
            XrCompileResourceStats committed;CHECK(xr_compile_resources_stats(resources,&committed)==XR_COMPILE_RESOURCE_OK);
            CHECK(committed.work-writes[i].work==2+expected_rename);
            CHECK(committed.allocation_count==writes[i].allocation_count);
        }
        CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK);xr_compile_resources_release(resources);
    }
    CHECK(writes[1].work-writes[0].work==1+sizeof(payload)-1);
    printf("independent write work=%zu, commit=2+FILE_RENAME_INFO bytes PASS\n",sizeof(payload));
}
#if PUBLICATION_INJECTED
static void cleanup_failures(const char *path) {
    for(unsigned published=0;published<2;++published)for(unsigned kind=0;kind<3;++kind) {
        if(published && !kind)continue;
        old_file(path);XrCompileResources *resources=NULL;XtcXirPublication *owner=ready(path,&resources);
        if(published)CHECK(xtc_xir_publication_commit(owner)==XTC_XIR_PUBLICATION_OK);
        XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
        close_attempts=0;fail_disposition=kind==0;fail_close=kind?kind-1:SIZE_MAX;
        CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_PENDING && owner);
        const XtcXirPublicationDiagnostic *d=xtc_xir_publication_diagnostic(owner);
        CHECK(d->published==(published!=0) && d->cleanup_pending && d->cleanup_os_error==ERROR_ACCESS_DENIED);
        if(published)CHECK(xtc_xir_publication_phase(owner)==XTC_XIR_PUBLICATION_PUBLISHED);
        CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.work==before.work && after.allocation_count==before.allocation_count);
        fail_disposition=false;fail_close=SIZE_MAX;
        CHECK(xtc_xir_publication_close(&owner)==XTC_XIR_PUBLICATION_OK && !owner);
        xr_compile_resources_release(resources);CHECK(!live_allocations);
        file_is(path,published?payload:"OLD",published?sizeof(payload)-1:3);
    }
}
static void failures(const char *path) {
    XrCompileResourceStats stats;allocation_attempts=0;io_attempts=0;work_count=0;record_work=true;
    CHECK(replace(path,true,&stats)==XTC_XIR_PUBLICATION_OK);record_work=false;
    size_t allocations=allocation_attempts,ios=io_attempts,points=work_count;
    for(size_t i=0;i<allocations;++i) {
        old_file(path);allocation_attempts=0;fail_allocation=i;
        CHECK(replace(path,true,&stats)==XTC_XIR_PUBLICATION_OUT_OF_MEMORY);file_is(path,"OLD",3);
    }
    fail_allocation=SIZE_MAX;
    for(size_t i=0;i<ios;++i)for(unsigned error=0;error<3;++error) {
        old_file(path);io_attempts=0;fail_io=i;
        injected_error=error==0?ERROR_ACCESS_DENIED:error==1?ERROR_NOT_ENOUGH_MEMORY:ERROR_OUTOFMEMORY;
        CHECK(replace(path,true,&stats)==(error?XTC_XIR_PUBLICATION_OUT_OF_MEMORY:XTC_XIR_PUBLICATION_IO));file_is(path,"OLD",3);
    }
    fail_io=SIZE_MAX;
    CHECK(replace(path,true,&stats)==XTC_XIR_PUBLICATION_OK);
    XrCompileResourceStats exact=stats;
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        limits=(XrCompileResourceLimits){UINT64_MAX,UINT64_MAX,UINT64_MAX};
        if(axis==0)limits.allocated_bytes=exact.allocated_bytes-minus;
        if(axis==1)limits.live_bytes=exact.peak_bytes-minus;
        if(axis==2)limits.work=exact.work-minus;
        old_file(path);CHECK(replace(path,true,&stats)==(minus?XTC_XIR_PUBLICATION_BUDGET:XTC_XIR_PUBLICATION_OK));
        file_is(path,minus?"OLD":payload,minus?3:sizeof(payload)-1);
    }
    limits=(XrCompileResourceLimits){UINT64_MAX,UINT64_MAX,UINT64_MAX};
    for(size_t i=0;i<points;++i) {
        old_file(path);limits.work=work_points[i]-1;
        CHECK(replace(path,true,&stats)==XTC_XIR_PUBLICATION_BUDGET);file_is(path,"OLD",3);
    }
    limits.work=UINT64_MAX;
    collision=true;old_file(path);CHECK(replace(path,true,&stats)==XTC_XIR_PUBLICATION_IO);collision=false;file_is(path,"OLD",3);
    zero_write=true;CHECK(replace(path,true,&stats)==XTC_XIR_PUBLICATION_IO);zero_write=false;file_is(path,"OLD",3);
    short_write=true;CHECK(replace(path,true,&stats)==XTC_XIR_PUBLICATION_OK);short_write=false;file_is(path,payload,sizeof(payload)-1);
    cleanup_failures(path);
    printf("%zu actual OOM, %zu Win32 IO/OOM points, %zu work boundaries; cumulative/peak exact-minus1 and cleanup PASS\n",allocations,ios,points);
}
#endif
int main(int argc,char **argv) {
    CHECK(argc==2);XrCompileResourceStats stats={0};DWORD before=winapi_control(),after=0;
    CHECK(replace(argv[1],true,&stats)==XTC_XIR_PUBLICATION_OK);
    old_file(argv[1]);CHECK(replace(argv[1],false,&stats)==XTC_XIR_PUBLICATION_OK);file_is(argv[1],"OLD",3);
    lifetime_and_limits(argv[1]);
    lease_sharing(argv[1]);operation_work(argv[1]);
#if PUBLICATION_INJECTED
    failures(argv[1]);
#endif
    CHECK(replace(argv[1],true,&stats)==XTC_XIR_PUBLICATION_OK);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
    fprintf(stderr,"publication owner handles before=%lu after=%lu\n",before,after);CHECK(before==after);
    printf("held output, producer release, discard and physical handles %lu/%lu PASS\n",before,after);return 0;
}
