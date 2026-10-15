/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_image_collector.c - Real image identities, leases and resource failures
 */
#ifdef IMAGE_HELPER_DLL
__declspec(dllexport) int image_answer(void) { return 42; }
#else
#include "app/toolchain/xtc_xir_images.h"
#include "app/toolchain/xtc_process.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s (Win32 %lu)\n",__LINE__,#c,GetLastError()); exit(1); } } while (0)
typedef struct Allocation { void *memory;size_t bytes; } Allocation;
static Allocation allocations[65536];
static size_t attempts,fail_at=SIZE_MAX,live,bytes_live,bytes_total,bytes_peak;
static void *observed_malloc(size_t bytes) {
    if(attempts++==fail_at)return NULL;
    void *memory=xr_malloc(bytes);if(!memory)return NULL;
    for(size_t i=0;i<65536;++i)if(!allocations[i].memory) {
        allocations[i]=(Allocation){memory,bytes};++live;bytes_live+=bytes;bytes_total+=bytes;
        if(bytes_live>bytes_peak)bytes_peak=bytes_live;return memory;
    }
    CHECK(false);return NULL;
}
static void observed_free(void *memory) {
    if(!memory)return;
    for(size_t i=0;i<65536;++i)if(allocations[i].memory==memory) {
        --live;bytes_live-=allocations[i].bytes;allocations[i]=(Allocation){0};xr_free(memory);return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_malloc
#define xr_free observed_free
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
static size_t io_attempts,io_fail_at=SIZE_MAX;
static size_t owned_handles;
static DWORD io_error=ERROR_READ_FAULT;
static bool short_read;
static bool replace_after_path;
static const char *replace_source,*replace_saved;
#ifndef IMAGE_PRODUCTION
static void put_file(const char *path,const char *text) {
    FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(text,1,strlen(text),f)==strlen(text));CHECK(!fclose(f));
}
static bool fail_io(void) {
    if(io_attempts++!=io_fail_at)return false;SetLastError(io_error);return true;
}
static HANDLE owner_handles[8192];
static size_t handles_acquired,handles_closed;
static void remember_handle(HANDLE handle) {
    for(size_t i=0;i<8192;++i)if(!owner_handles[i]) {owner_handles[i]=handle;++owned_handles;++handles_acquired;return;}
    CHECK(false);
}
static HANDLE observed_create(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES security,
    DWORD creation,DWORD flags,HANDLE template_file) {
    if(fail_io())return INVALID_HANDLE_VALUE;
    HANDLE handle=CreateFileW(name,access,share,security,creation,flags,template_file);
    if(handle!=INVALID_HANDLE_VALUE)remember_handle(handle);return handle;
}
static BOOL observed_duplicate(HANDLE source_process,HANDLE source,HANDLE target_process,LPHANDLE target,
    DWORD access,BOOL inherit,DWORD options) {
    if(fail_io())return FALSE;
    BOOL ok=DuplicateHandle(source_process,source,target_process,target,access,inherit,options);
    if(ok)remember_handle(*target);return ok;
}
static BOOL observed_close(HANDLE handle) {
    for(size_t i=0;i<8192;++i)if(owner_handles[i]==handle) {
        CHECK(CloseHandle(handle));owner_handles[i]=NULL;--owned_handles;++handles_closed;return TRUE;
    }
    CHECK(false);return FALSE;
}
static DWORD observed_path(HANDLE handle,LPWSTR path,DWORD capacity,DWORD flags) {
    if(fail_io())return 0;
    DWORD result=GetFinalPathNameByHandleW(handle,path,capacity,flags);
    if(result&&capacity&&replace_after_path) {
        replace_after_path=false;CHECK(MoveFileA(replace_source,replace_saved));put_file(replace_source,"replacement");
    }
    return result;
}
static BOOL observed_read(HANDLE file,LPVOID bytes,DWORD count,LPDWORD actual,LPOVERLAPPED overlapped) {
    if(fail_io())return FALSE;
    BOOL ok=ReadFile(file,bytes,count,actual,overlapped);
    if(ok&&short_read&&*actual)--*actual;
    return ok;
}
#define CreateFileW observed_create
#define CloseHandle observed_close
#define GetFileInformationByHandle(...) (fail_io()?FALSE:GetFileInformationByHandle(__VA_ARGS__))
#define GetFileInformationByHandleEx(...) (fail_io()?FALSE:GetFileInformationByHandleEx(__VA_ARGS__))
#define GetFinalPathNameByHandleW observed_path
#define GetFileType(...) (fail_io()?FILE_TYPE_UNKNOWN:GetFileType(__VA_ARGS__))
#define MultiByteToWideChar(...) (fail_io()?0:MultiByteToWideChar(__VA_ARGS__))
#define WideCharToMultiByte(...) (fail_io()?0:WideCharToMultiByte(__VA_ARGS__))
#define CompareStringOrdinal(...) (fail_io()?0:CompareStringOrdinal(__VA_ARGS__))
#define GetFileSizeEx(...) (fail_io()?FALSE:GetFileSizeEx(__VA_ARGS__))
#define ReadFile observed_read
#define SetFilePointerEx(...) (fail_io()?FALSE:SetFilePointerEx(__VA_ARGS__))
#define DuplicateHandle observed_duplicate
#include "app/toolchain/xtc_xir_target.c"
#include "app/toolchain/xtc_xir_sysroot.c"
#include "app/toolchain/xtc_xir_images.c"
#undef CloseHandle
#undef CreateFileW
#undef GetFileInformationByHandle
#undef GetFileInformationByHandleEx
#undef GetFinalPathNameByHandleW
#undef GetFileType
#undef MultiByteToWideChar
#undef WideCharToMultiByte
#undef CompareStringOrdinal
#undef GetFileSizeEx
#undef ReadFile
#undef SetFilePointerEx
#undef DuplicateHandle
#endif
static const XrCompileResourceLimits unlimited={UINT64_MAX,UINT64_MAX,UINT64_MAX};
static const uint8_t abc[32]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
static DWORD handle_count(void) { DWORD count=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&count));return count; }
static XrCompileResources *ledger(const XrCompileResourceLimits *limits) {
    CHECK(!live&&!bytes_live);attempts=bytes_total=bytes_peak=0;
    XrCompileResources *r=NULL;CHECK(xr_compile_resources_new(limits,&r)==XR_COMPILE_RESOURCE_OK);return r;
}
static XrCompileResourceStats stats(XrCompileResources *r) {
    XrCompileResourceStats s;CHECK(xr_compile_resources_stats(r,&s)==XR_COMPILE_RESOURCE_OK);
    CHECK(s.live_bytes==bytes_live&&s.allocated_bytes==bytes_total&&s.peak_bytes==bytes_peak);return s;
}
static XrXirImageCollector *make_images(XrCompileResources *r) {
    XrXirImageCollector *images=NULL;CHECK(xtc_xir_images_new(r,&images)==XR_XIR_TARGET_OK);
    CHECK(xtc_xir_images_resources(images)==r&&!xtc_xir_images_sealed(images)&&!xtc_xir_images_count(images));return images;
}
static HANDLE open_event(const char *path) {
    HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    CHECK(file!=INVALID_HANDLE_VALUE);return file;
}
static XrOsProcStatus observe(XrXirImageCollector *images,HANDLE file,XrProcImageKind kind) {
    XrProcImageObserver observer=xtc_xir_images_observer(images);
    XrProcImageEvent event={1,kind,(intptr_t)file};return observer.observe(observer.context,&event);
}
static XrXirTargetSnapshotRequest request(XrCompileResources *r,const XrXirImageCollector *images,
    const XrXirTargetDependency *dependencies,uint32_t count) {
    static const char *args[]={"observed-file"};
    static const XrXirTargetCommandFacts command={"C:/described-tool.exe","C:/",args,1,NULL,0,3000,1048576,0,0,0};
    return (XrXirTargetSnapshotRequest){r,"x86_64-windows-msvc",3,2,11,dependencies,count,&command,1,images};
}
static void blocked_writer(const char *path) {
    HANDLE file=CreateFileA(path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    CHECK(file==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION);
    CHECK(!DeleteFileA(path)&&GetLastError()==ERROR_SHARING_VIOLATION);
}
static XrXirTargetStatus workflow(XrCompileResources *r,HANDLE event,const char *path,XrXirTargetSnapshot **out) {
    XrXirImageCollector *images=NULL;XrXirTargetStatus s=xtc_xir_images_new(r,&images);
    if(s==XR_XIR_TARGET_OK) {
        if(observe(images,event,XR_PROC_IMAGE_EXECUTABLE)!=XR_PROC_OK)s=xtc_xir_images_status(images);
        if(s==XR_XIR_TARGET_OK&&observe(images,event,XR_PROC_IMAGE_DLL)!=XR_PROC_OK)s=xtc_xir_images_status(images);
        if(s==XR_XIR_TARGET_OK)s=xtc_xir_images_seal(images);
        if(s==XR_XIR_TARGET_OK) {
            void *bytes=NULL;size_t length=0;
            s=xtc_xir_images_read(images,0,3,&bytes,&length);
            if(s==XR_XIR_TARGET_OK)CHECK(length==3&&!memcmp(bytes,"abc",3));
            else CHECK(!bytes&&!length);
            xr_compile_resources_free(bytes);
        }
        if(s==XR_XIR_TARGET_OK) {
            XrXirTargetDependency input={path,XR_XIR_TARGET_COMPILER};XrXirTargetSnapshotRequest spec=request(r,images,&input,1);
            s=xtc_xir_target_snapshot_capture(&spec,out);
        }
    }
    xtc_xir_images_free(images);return s;
}
static void matrix(const char *path) {
    HANDLE event=open_event(path);DWORD baseline=handle_count();XrCompileResources *r=ledger(&unlimited);
    XrXirTargetSnapshot *snapshot=NULL;attempts=io_attempts=0;
    CHECK(workflow(r,event,path,&snapshot)==XR_XIR_TARGET_OK);
    size_t mallocs=attempts,calls=io_attempts;XrCompileResourceStats total=stats(r);
    xtc_xir_target_free(snapshot);xr_compile_resources_release(r);CHECK(!live&&!bytes_live&&!owned_handles&&handle_count()==baseline);
#ifndef IMAGE_PRODUCTION
    for(size_t i=0;i<mallocs;++i) {
        r=ledger(&unlimited);attempts=0;fail_at=i;snapshot=NULL;
        CHECK(workflow(r,event,path,&snapshot)==XR_XIR_TARGET_OUT_OF_MEMORY&&!snapshot);
        fail_at=SIZE_MAX;CHECK(attempts>i);xr_compile_resources_release(r);
        CHECK(!live&&!bytes_live&&!owned_handles&&handle_count()==baseline);
    }
    for(unsigned memory=0;memory<2;++memory) for(size_t i=0;i<calls;++i) {
        r=ledger(&unlimited);io_attempts=0;io_fail_at=i;io_error=memory?ERROR_NOT_ENOUGH_MEMORY:ERROR_READ_FAULT;snapshot=NULL;
        CHECK(workflow(r,event,path,&snapshot)==(memory?XR_XIR_TARGET_OUT_OF_MEMORY:XR_XIR_TARGET_IO)&&!snapshot);
        io_fail_at=SIZE_MAX;CHECK(io_attempts==i+1);xr_compile_resources_release(r);
        CHECK(!live&&!bytes_live&&!owned_handles&&handle_count()==baseline);
    }
#endif
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus) {
        XrCompileResourceLimits limits=unlimited;
        if(axis==0)limits.allocated_bytes=total.allocated_bytes-minus;
        if(axis==1)limits.live_bytes=total.peak_bytes-minus;
        if(axis==2)limits.work=total.work-minus;
        r=ledger(&limits);snapshot=NULL;
        CHECK(workflow(r,event,path,&snapshot)==(minus?XR_XIR_TARGET_BUDGET:XR_XIR_TARGET_OK));
        CHECK(minus?!snapshot:snapshot!=NULL);xtc_xir_target_free(snapshot);xr_compile_resources_release(r);
        CHECK(!live&&!bytes_live&&!owned_handles&&handle_count()==baseline);
    }
    CHECK(CloseHandle(event));printf("image owner: %zu malloc, %zu IO/OOM; physical formula, three axes exact/minus1 PASS\n",mallocs,calls);
}
static XrXirImageCollector *scalar_images(XrCompileResources *r,HANDLE event) {
    XrXirImageCollector *images=make_images(r);
    CHECK(observe(images,event,XR_PROC_IMAGE_EXECUTABLE)==XR_PROC_OK);
    CHECK(xtc_xir_images_seal(images)==XR_XIR_TARGET_OK);return images;
}
static void scalar_stopped(XrXirImageCollector *images,XrCompileResources *r,XrXirTargetStatus status) {
    uint32_t value=0xfeedface;size_t mallocs=attempts,calls=io_attempts;XrCompileResourceStats before=stats(r);
    CHECK(xtc_xir_images_read_scalar(images,0,0,4,&value)==status&&value==0xfeedface);
    CHECK(attempts==mallocs&&io_attempts==calls&&stats(r).work==before.work);
}
static void scalar_faults(HANDLE event) {
#ifndef IMAGE_PRODUCTION
    for(unsigned error=0;error<3;++error)for(size_t cut=0;cut<(error==2?1u:2u);++cut) {
        XrCompileResources *r=ledger(&unlimited);XrXirImageCollector *images=scalar_images(r,event);
        size_t mallocs=attempts,calls=io_attempts;uint32_t value=0xfeedface;
        io_error=error==1?ERROR_NOT_ENOUGH_MEMORY:ERROR_READ_FAULT;
        if(error==2)short_read=true;else io_fail_at=calls+cut;
        XrXirTargetStatus expected=error==1?XR_XIR_TARGET_OUT_OF_MEMORY:XR_XIR_TARGET_IO;
        CHECK(xtc_xir_images_read_scalar(images,0,0,4,&value)==expected&&value==0xfeedface);
        io_fail_at=SIZE_MAX;short_read=false;
        CHECK(attempts==mallocs&&io_attempts==calls+(error==2?2:cut+1));
        scalar_stopped(images,r,expected);xtc_xir_images_free(images);xr_compile_resources_release(r);
        CHECK(!live&&!bytes_live&&!owned_handles);
    }
#else
    (void)event;
#endif
}
static void scalar_budgets(HANDLE event,const XrCompileResourceStats *total,uint64_t setup_work) {
    for(unsigned axis=0;axis<3;++axis)for(unsigned edge=0;edge<3;++edge) {
        XrCompileResourceLimits limits=unlimited;uint64_t bound=edge==0?0:edge==1?1:2;
        if(axis==0)limits.allocated_bytes=total->allocated_bytes-1+bound;
        if(axis==1)limits.live_bytes=total->peak_bytes-1+bound;
        if(axis==2)limits.work=total->work-1+bound;
        XrCompileResources *r=ledger(&limits);XrXirImageCollector *images=NULL;
        XrXirTargetStatus status=xtc_xir_images_new(r,&images);uint32_t value=0xfeedface;
        if(status==XR_XIR_TARGET_OK&&observe(images,event,XR_PROC_IMAGE_EXECUTABLE)!=XR_PROC_OK)status=xtc_xir_images_status(images);
        if(status==XR_XIR_TARGET_OK)status=xtc_xir_images_seal(images);
        if(status==XR_XIR_TARGET_OK)status=xtc_xir_images_read_scalar(images,0,0,4,&value);
        CHECK(status==(edge?XR_XIR_TARGET_OK:XR_XIR_TARGET_BUDGET));
        CHECK(value==(edge?0x80636261u:0xfeedfaceu));xtc_xir_images_free(images);xr_compile_resources_release(r);
        CHECK(!live&&!bytes_live&&!owned_handles);
    }
    for(uint64_t work=setup_work;work<total->work;++work) {
        XrCompileResourceLimits limits=unlimited;limits.work=work;
        XrCompileResources *r=ledger(&limits);XrXirImageCollector *images=scalar_images(r,event);
        uint32_t value=0xfeedface;size_t mallocs=attempts;
        CHECK(xtc_xir_images_read_scalar(images,0,0,4,&value)==XR_XIR_TARGET_BUDGET&&value==0xfeedface);
        CHECK(attempts==mallocs);scalar_stopped(images,r,XR_XIR_TARGET_BUDGET);
        xtc_xir_images_free(images);xr_compile_resources_release(r);CHECK(!live&&!bytes_live&&!owned_handles);
    }
}
static void scalar_cases(const char *path) {
    char name[32768];CHECK(snprintf(name,sizeof(name),"%s.scalar",path)>0);
    static const unsigned char bytes[]={'a','b','c',0x80};FILE *f=fopen(name,"wb");CHECK(f);
    CHECK(fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes)&&!fclose(f));
    DWORD handles=handle_count();HANDLE event=open_event(name);XrCompileResources *r=ledger(&unlimited);
    XrXirImageCollector *images=make_images(r);uint32_t value=0xfeedface;
    XrCompileResourceStats before=stats(r);size_t mallocs=attempts,calls=io_attempts;
    CHECK(xtc_xir_images_read_scalar(images,0,0,4,&value)==XR_XIR_TARGET_INVALID&&value==0xfeedface);
    CHECK(stats(r).work==before.work&&attempts==mallocs&&io_attempts==calls);xtc_xir_images_free(images);xr_compile_resources_release(r);
    r=ledger(&unlimited);images=scalar_images(r,event);uint64_t setup_work=stats(r).work;mallocs=attempts;
    CHECK(xtc_xir_images_read_scalar(images,0,0,4,&value)==XR_XIR_TARGET_OK&&value==0x80636261u);
    CHECK(attempts==mallocs);XrCompileResourceStats total=stats(r);
    before=stats(r);calls=io_attempts;
    CHECK(xtc_xir_images_read_scalar(images,1,0,1,&value)==XR_XIR_TARGET_INVALID);
    CHECK(xtc_xir_images_read_scalar(images,0,0,3,&value)==XR_XIR_TARGET_INVALID);
    CHECK(xtc_xir_images_read_scalar(images,0,4,1,&value)==XR_XIR_TARGET_INVALID);
    CHECK(xtc_xir_images_read_scalar(images,0,UINT64_MAX,2,&value)==XR_XIR_TARGET_INVALID);
    CHECK(xtc_xir_images_read_scalar(images,0,0,1,NULL)==XR_XIR_TARGET_INVALID);
    CHECK(value==0x80636261u&&stats(r).work==before.work&&attempts==mallocs&&io_attempts==calls);
    xr_compile_resources_release(r);CHECK(CloseHandle(event));blocked_writer(name);
    CHECK(xtc_xir_images_read_scalar(images,0,1,2,&value)==XR_XIR_TARGET_OK&&value==0x6362);
    CHECK(xtc_xir_images_read_scalar(images,0,3,1,&value)==XR_XIR_TARGET_OK&&value==0x80);
    CHECK(attempts==mallocs);xtc_xir_images_free(images);CHECK(!live&&!bytes_live&&!owned_handles);
    event=open_event(name);scalar_faults(event);scalar_budgets(event,&total,setup_work);CHECK(CloseHandle(event));
    CHECK(DeleteFileA(name)&&handle_count()==handles);puts("image scalars: same lease, no allocation, all I/O failures and work cuts, three axes exact/minus1/plus1 PASS");
}
static void identities(const char *path,const char *alias,const char *saved) {
    DWORD baseline=handle_count();XrCompileResources *r=ledger(&unlimited);XrXirImageCollector *images=make_images(r);
    CHECK(xtc_xir_images_seal(images)==XR_XIR_TARGET_INVALID&&!xtc_xir_images_sealed(images));xtc_xir_images_free(images);
    images=make_images(r);CHECK(observe(images,NULL,XR_PROC_IMAGE_DLL)==XR_PROC_UNSUPPORTED);
    CHECK(xtc_xir_images_seal(images)==XR_XIR_TARGET_UNSUPPORTED);xtc_xir_images_free(images);
    CHECK(CreateHardLinkA(alias,path,NULL));
    images=make_images(r);HANDLE event=open_event(path);LARGE_INTEGER pos;pos.QuadPart=2;
    CHECK(SetFilePointerEx(event,pos,NULL,FILE_BEGIN));
    CHECK(observe(images,event,XR_PROC_IMAGE_EXECUTABLE)==XR_PROC_OK);
    CHECK(observe(images,event,XR_PROC_IMAGE_DLL)==XR_PROC_OK);
    CHECK(!xtc_xir_images_count(images)&&!xtc_xir_images_file(images,0));
    void *owned=NULL;size_t owned_length=0;size_t invalid_attempts=attempts;
    CHECK(xtc_xir_images_read(images,0,3,&owned,&owned_length)==XR_XIR_TARGET_INVALID);
    CHECK(!owned&&!owned_length&&attempts==invalid_attempts);
    HANDLE second=open_event(alias);
    CHECK(observe(images,second,XR_PROC_IMAGE_DLL)==XR_PROC_OK);CHECK(CloseHandle(second));
    xr_compile_resources_release(r); /* Collector allocations retain the original ledger. */
#ifndef IMAGE_PRODUCTION
    uint64_t seal_before=stats(r).work;
#endif
    CHECK(xtc_xir_images_seal(images)==XR_XIR_TARGET_OK&&xtc_xir_images_count(images)==2);
#ifndef IMAGE_PRODUCTION
    /* Two 3-byte files: each size/init/read/hash/final costs 1+1+4+3+1.
     * The single result allocation charges its header and zeroed payload. */
    CHECK(stats(r).work-seal_before==1+sizeof(XtcXirMemory)+2*sizeof(XrXirImageFile)+2*(10+sizeof(XrXirImageFile))+1);
#endif
    pos.QuadPart=0;CHECK(SetFilePointerEx(event,pos,&pos,FILE_CURRENT)&&pos.QuadPart==2);CHECK(CloseHandle(event));
    XrCompileResourceStats before=stats(r);CHECK(xtc_xir_images_seal(images)==XR_XIR_TARGET_OK);
    XrProcImageObserver callback=xtc_xir_images_observer(images);
    CHECK(callback.observe(callback.context,(const XrProcImageEvent *)(uintptr_t)1)==XR_PROC_INVALID_ARGUMENT);
    CHECK(xtc_xir_images_status(images)==XR_XIR_TARGET_OK&&stats(r).work==before.work);
    CHECK(xtc_xir_images_file(images,0)->kind_mask==(XR_XIR_IMAGE_EXE|XR_XIR_IMAGE_DLL));
    for(uint32_t i=0;i<2;++i)CHECK(xtc_xir_images_file(images,i)->length==3&&!memcmp(xtc_xir_images_file(images,i)->digest,abc,32));
    CHECK(!xtc_xir_images_file(images,2));blocked_writer(path);
    invalid_attempts=attempts;
    CHECK(xtc_xir_images_read(images,2,3,&owned,&owned_length)==XR_XIR_TARGET_INVALID);
    owned=(void *)(uintptr_t)1;
    CHECK(xtc_xir_images_read(images,0,3,&owned,&owned_length)==XR_XIR_TARGET_INVALID);
    CHECK(owned==(void *)(uintptr_t)1&&!owned_length);owned=NULL;owned_length=1;
    CHECK(xtc_xir_images_read(images,0,3,&owned,&owned_length)==XR_XIR_TARGET_INVALID);
    CHECK(!owned&&owned_length==1&&attempts==invalid_attempts);owned_length=0;
    CHECK(xtc_xir_images_read(images,0,3,&owned,&owned_length)==XR_XIR_TARGET_OK);
    CHECK(owned_length==3&&!memcmp(owned,"abc",3));
    for(uint32_t i=0;i<2;++i) {
        void *again=NULL;size_t length=0;
        CHECK(xtc_xir_images_read(images,i,3,&again,&length)==XR_XIR_TARGET_OK);
        CHECK(length==3&&!memcmp(again,"abc",3));xr_compile_resources_free(again);
    }
    XrXirTargetSnapshot *snapshot=NULL;XrXirTargetDependency file={path,XR_XIR_TARGET_HEADER};XrXirTargetSnapshotRequest spec=request(r,images,&file,1);
    CHECK(xtc_xir_target_snapshot_capture(&spec,&snapshot)==XR_XIR_TARGET_INVALID&&!snapshot);
    XrCompileResources *foreign=NULL;CHECK(xr_compile_resources_new(&unlimited,&foreign)==XR_COMPILE_RESOURCE_OK);
    spec.resources=foreign;size_t count=attempts;CHECK(xtc_xir_target_snapshot_capture(&spec,&snapshot)==XR_XIR_TARGET_INVALID&&!snapshot&&attempts==count);
    xr_compile_resources_release(foreign);spec.resources=r;file.kind=XR_XIR_TARGET_COMPILER;
    CHECK(xtc_xir_target_snapshot_capture(&spec,&snapshot)==XR_XIR_TARGET_OK);
    CHECK(xtc_xir_target_facts(snapshot)->file_count==2);xtc_xir_images_free(images);
    blocked_writer(path);blocked_writer(alias);
    for(uint32_t i=0;i<2;++i)CHECK(xtc_xir_target_file(snapshot,i)->length==3&&!memcmp(xtc_xir_target_file(snapshot,i)->digest,abc,32));
    xtc_xir_target_free(snapshot);
    CHECK(owned_length==3&&!memcmp(owned,"abc",3));xr_compile_resources_free(owned);
    CHECK(!live&&!bytes_live&&!owned_handles&&handle_count()==baseline);CHECK(DeleteFileA(alias));
    r=ledger(&unlimited);images=make_images(r);event=open_event(path);
    CHECK(observe(images,event,XR_PROC_IMAGE_EXECUTABLE)==XR_PROC_OK);
    CHECK(xtc_xir_images_seal(images)==XR_XIR_TARGET_OK);CHECK(CloseHandle(event));
    owned=NULL;owned_length=0;invalid_attempts=attempts;
    CHECK(xtc_xir_images_read(images,0,2,&owned,&owned_length)==XR_XIR_TARGET_BUDGET);
    CHECK(!owned&&!owned_length&&attempts==invalid_attempts);
    CHECK(xtc_xir_images_read(images,0,3,&owned,&owned_length)==XR_XIR_TARGET_BUDGET);
    xtc_xir_images_free(images);xr_compile_resources_release(r);
    CHECK(!live&&!bytes_live&&!owned_handles&&handle_count()==baseline);
#ifndef IMAGE_PRODUCTION
    r=ledger(&unlimited);images=make_images(r);event=open_event(path);
    replace_source=path;replace_saved=saved;replace_after_path=true;
    CHECK(observe(images,event,XR_PROC_IMAGE_EXECUTABLE)==XR_PROC_INVALID_ARGUMENT);
    CHECK(xtc_xir_images_status(images)==XR_XIR_TARGET_INVALID&&!xtc_xir_images_count(images));
    xtc_xir_images_free(images);CHECK(CloseHandle(event));xr_compile_resources_release(r);
    CHECK(DeleteFileA(path));CHECK(MoveFileA(saved,path));CHECK(!live&&!bytes_live&&!owned_handles&&handle_count()==baseline);
    puts("actual file ID replacement race and independent seal work formula PASS");
#else
    (void)saved;(void)replace_source;(void)replace_saved;(void)replace_after_path;
#endif
    puts("hardlink aliases, same-handle reads, producer-dead bytes, cursor and sealed immutability PASS");
}
/* This independent WinAPI control reproduces first-use OS-owned state without
 * invoking any Xray process/collector owner. Preserve its initial delta; only
 * subsequent owner scopes assert a zero process-handle delta. */
static void native_control(const char *executable,const char *argument) {
    DWORD initial=handle_count(),first=0;
    for(unsigned repeat=0;repeat<2;++repeat) {
        HANDLE read,write;SECURITY_ATTRIBUTES security={sizeof(security),NULL,TRUE};
        for(unsigned i=0;i<2;++i) {CHECK(CreatePipe(&read,&write,&security,0));CHECK(CloseHandle(read));CHECK(CloseHandle(write));}
        char command[32768];CHECK(snprintf(command,sizeof(command),"\"%s\" %s",executable,argument)>0);
        STARTUPINFOA startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION process={0};
        CHECK(CreateProcessA(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process));
        DWORD waited=WaitForSingleObject(process.hProcess,10000);
        if(waited!=WAIT_OBJECT_0) {CHECK(TerminateProcess(process.hProcess,1));CHECK(WaitForSingleObject(process.hProcess,10000)==WAIT_OBJECT_0);}
        DWORD code=1;CHECK(waited==WAIT_OBJECT_0);
        CHECK(GetExitCodeProcess(process.hProcess,&code)&&!code);CHECK(CloseHandle(process.hThread));CHECK(CloseHandle(process.hProcess));
        if(!repeat)first=handle_count();else CHECK(handle_count()==first);
    }
    printf("native WinAPI external boundary: initial=%lu first=%lu repeated=%lu\n",initial,first,handle_count());
}
static void child(const char *self,const char *library) {
    for(unsigned i=0;i<2;++i) {
        HMODULE dll=LoadLibraryA(library);CHECK(dll);FARPROC proc=GetProcAddress(dll,"image_answer");CHECK(proc);
        int(*answer)(void)=NULL;memcpy(&answer,&proc,sizeof(answer));CHECK(answer()==42);CHECK(FreeLibrary(dll));
    }
    char command[32768];CHECK(snprintf(command,sizeof(command),"\"%s\" --leaf",self)>0);
    STARTUPINFOA startup={0};startup.cb=sizeof(startup);PROCESS_INFORMATION process={0};
    startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    /* Keep the real descendant headless; allocating a new desktop console can
     * activate the host's packaged terminal COM server outside this fixture. */
    CHECK(CreateProcessA(self,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process));
    CHECK(WaitForSingleObject(process.hProcess,10000)==WAIT_OBJECT_0);DWORD code=1;
    CHECK(GetExitCodeProcess(process.hProcess,&code)&&!code);CHECK(CloseHandle(process.hThread));CHECK(CloseHandle(process.hProcess));
    puts("image-child 42");
}
typedef struct ProcessTest { int argc; char **argv; bool command; } ProcessTest;
static XrOsProcStatus process_image_diagnostic(void *context,const XrProcImageEvent *event) {
    XrProcImageObserver *inner=context;
    XrOsProcStatus status=inner->observe(inner->context,event);
    if(status!=XR_PROC_OK) {
        DWORD error=GetLastError();wchar_t path[32768];
        DWORD length=GetFinalPathNameByHandleW((HANDLE)event->file_handle,path,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        fprintf(stderr,"image rejection pid=%lld kind=%u status=%u error=%lu path=%ls\n",
            (long long)event->pid,(unsigned)event->kind,(unsigned)status,error,length&&length<32768?path:L"<unavailable>");
        SetLastError(error);
    }
    return status;
}
static void real_process(const ProcessTest *test) {
    char **argv=test->argv;
    DWORD baseline=handle_count();XrCompileResources *r=ledger(&unlimited);XrXirImageCollector *images=make_images(r);
    XrProcessSpec spec;xtc_process_spec_init(&spec,test->command?argv[4]:argv[0],30000);
    if(test->command) {
        CHECK(test->argc-4<XTC_PROCESS_MAX_ARGS);
        for(int i=4;i<test->argc;++i)spec.argv[i-4]=argv[i];spec.cwd=argv[3];
    } else {spec.argv[1]="--child";spec.argv[2]=argv[4];spec.cwd=argv[1];}
    spec.environment_source=XTC_PROCESS_ENV_SNAPSHOT;spec.image_mode=XR_PROC_IMAGES_WINDOWS_TREE;
    XrProcImageObserver inner=xtc_xir_images_observer(images);
    spec.image_observer=(XrProcImageObserver){&inner,process_image_diagnostic};XrToolchainProcess *process=NULL;
    CHECK(xtc_process_prepare(r,&spec,&process)==XTC_PROCESS_OK);XrProcessResult result={0};
    XrProcessStatus status=xtc_process_run(process,NULL,NULL,&result);
    if(status!=XTC_PROCESS_OK)fprintf(stderr,"real run %s collector %d\n",xtc_process_status_name(status),xtc_xir_images_status(images));
    if(result.exit_code) {
        if(result.stdout_bytes.length)fwrite(result.stdout_bytes.data,1,result.stdout_bytes.length,stderr);
        if(result.stderr_bytes.length)fwrite(result.stderr_bytes.data,1,result.stderr_bytes.length,stderr);
    }
    CHECK(status==XTC_PROCESS_OK&&result.exit_code==0);
    static const char expected[]="image-leaf\r\nimage-child 42\r\n";
    if(!test->command) {
        CHECK(result.stdout_bytes.length==sizeof(expected)-1&&!memcmp(result.stdout_bytes.data,expected,sizeof(expected)-1));
        CHECK(!result.stderr_bytes.length);
    }
    xtc_process_result_free(&result);
    CHECK(xtc_xir_images_seal(images)==XR_XIR_TARGET_OK);bool found=false;
    for(uint32_t i=0;i<xtc_xir_images_count(images);++i) {
        const XrXirImageFile *file=xtc_xir_images_file(images,i);
        const char *name=strrchr(file->path,'/');if(name&&!strcmp(name+1,"image_fixture.dll")) {found=true;CHECK(file->kind_mask&XR_XIR_IMAGE_DLL);}
        printf("IMAGE %u %llu ",file->kind_mask,(unsigned long long)file->length);
        for(unsigned b=0;b<32;++b)printf("%02x",file->digest[b]);printf(" %s\n",file->path);
    }
    CHECK(found||test->command);XrXirTargetSnapshot *snapshot=NULL;XrXirTargetSnapshotRequest target=request(r,images,NULL,0);
    XrProcessView view;CHECK(xtc_process_view(process,&view)==XTC_PROCESS_OK);
    XrXirTargetEnvironment environment[XTC_PROCESS_MAX_ENV];
    for(size_t i=0;i<view.env_count;++i)environment[i]=(XrXirTargetEnvironment){view.env_keys[i],view.env_values[i]};
    XrXirTargetCommandFacts command={view.executable,view.cwd,view.argv,(uint32_t)view.argc,environment,(uint32_t)view.env_count,
        view.timeout_ms,view.output_limit,(uint32_t)view.image_mode, (uint32_t)view.completion_policy, (uint32_t)view.extension_policy};
    /* Evidence contains only a framed identity of the complete environment. */
    XrSHA256Context env_hash;uint8_t env_digest[32];xr_sha256_init(&env_hash);
    static const char domain[]="test:frozen-environment:v1";
    xr_sha256_update(&env_hash,(const uint8_t *)domain,sizeof(domain));
    for(size_t i=0;i<view.env_count;++i) {
        xr_sha256_update(&env_hash,(const uint8_t *)view.env_keys[i],strlen(view.env_keys[i])+1);
        xr_sha256_update(&env_hash,(const uint8_t *)view.env_values[i],strlen(view.env_values[i])+1);
    }
    xr_sha256_final(&env_hash,env_digest);printf("FROZEN_ENV_SHA256 %zu ",view.env_count);
    for(unsigned i=0;i<32;++i)printf("%02x",env_digest[i]);putchar('\n');
    target.commands=&command;if(test->command)target.provider=(uint32_t)atoi(argv[2]);
    CHECK(xtc_xir_target_snapshot_capture(&target,&snapshot)==XR_XIR_TARGET_OK);
    xtc_process_free(process);xtc_xir_images_free(images);xr_compile_resources_release(r);
    CHECK(xtc_xir_target_command_facts(snapshot,0)->argc==view.argc);
    CHECK(xtc_xir_target_facts(snapshot)->file_count>2);xtc_xir_target_free(snapshot);
    fprintf(stderr,"image final: heap=%zu bytes=%zu handles=%lu baseline=%lu\n",live,bytes_live,handle_count(),baseline);
#ifndef IMAGE_PRODUCTION
    CHECK(handles_acquired==handles_closed);printf("collector acquired=%zu closed=%zu live=%zu\n",handles_acquired,handles_closed,owned_handles);
#endif
    CHECK(!live&&!bytes_live&&!owned_handles);puts("real image observation, frozen command and snapshot after all producers PASS");
}
static DWORD WINAPI process_thread(void *context) {
    real_process(context);return 0;
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--native-child"))return 0;
    if(argc==2&&!strcmp(argv[1],"--leaf")) {puts("image-leaf");return 0;}
    if(argc==3&&!strcmp(argv[1],"--child")) {child(argv[0],argv[2]);return 0;}
    bool command=argc>=5&&!strcmp(argv[1],"--observe-command");
    CHECK(command||argc==6);(void)io_error;(void)io_fail_at;(void)short_read;
    if(command||!strcmp(argv[5],"process")) {
        ProcessTest test={argc,argv,command};
        /* Provider diagnostics first measure a native version/help request.
         * This is an explicit external-state control, not collector execution. */
        const char *name=command?argv[4]:argv[0];
        for(const char *p=name;*p;++p)if(*p=='/'||*p=='\\')name=p+1;
        bool zig_control=command&&!strcmp(name,"zig.exe");
        native_control(zig_control?argv[4]:argv[0],zig_control?"version":"--native-child");
        DWORD baseline=handle_count();HANDLE thread=CreateThread(NULL,0,process_thread,&test,0,NULL);CHECK(thread);
        CHECK(WaitForSingleObject(thread,INFINITE)==WAIT_OBJECT_0);DWORD code=1;
        CHECK(GetExitCodeThread(thread,&code)&&!code);CHECK(CloseHandle(thread));
        fprintf(stderr,"image thread scope: handles=%lu baseline=%lu\n",handle_count(),baseline);
        CHECK(handle_count()==baseline);
    }
    else {identities(argv[2],argv[3],argv[4]);matrix(argv[2]);scalar_cases(argv[2]);}
    return 0;
}
#endif
