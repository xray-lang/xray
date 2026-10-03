/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_windows_path.c - Real long locators and caller-owned failure boundaries
 */
#include "base/xmalloc.h"
#include "base/xcompile_resources.h"
#include "base/xio_policy.h"
#include "base/xfileio.h"
#include "os/os_fs.h"
#include "os/os_dir.h"
#include "os/os_file_read.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s os=%lu\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
#define SOURCE_PATHS_PRODUCTION
#include "../../cli/source_paths_owner/source_paths_faults.inc.h"
#ifndef WINDOWS_PATH_PRODUCTION
static DWORD full_error;
static bool full_overflow;
static size_t full_calls;
static DWORD observed_full(LPCWSTR input, DWORD capacity, LPWSTR output, LPWSTR *part) {
    ++full_calls;
    if (full_error) { SetLastError(full_error); return 0; }
    if (full_overflow) return capacity;
    return GetFullPathNameW(input, capacity, output, part);
}
#define GetFullPathNameW observed_full
#endif
#include "base/xwindows_utf8.h"
#ifndef WINDOWS_PATH_PRODUCTION
#undef GetFullPathNameW
#endif
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static XrCompileResources *ledger(const XrCompileResourceLimits *limits) {
    XrCompileResources *r = NULL;
    CHECK(xr_compile_resources_new(limits, &r) == XR_COMPILE_RESOURCE_OK); return r;
}
static XrCompileResourceStats convert(const char *path, XrCompileResourceLimits limits, XrOsIoStatus expected) {
    CHECK(!live); total = peak = 0;
    XrCompileResources *r = NULL;
    XrCompileResourceStatus made = xr_compile_resources_new(&limits, &r);
    if (made != XR_COMPILE_RESOURCE_OK) {
        CHECK((made == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && expected == XR_OS_IO_OUT_OF_MEMORY) ||
            (made == XR_COMPILE_RESOURCE_BUDGET && expected == XR_OS_IO_BUDGET));
        CHECK(!r && !live); return (XrCompileResourceStats){0};
    }
    XrOsIoPolicy policy = xr_compile_io_policy(r);
    wchar_t *out = (wchar_t *)1;
    XrOsIoStatus status = xr_win_utf8_path_owned(&policy, path, &out);
    if (status != expected) fprintf(stderr,"conversion got %d expected %d\n",status,expected);
    CHECK(status == expected);
    if (status != XR_OS_IO_OK) CHECK(out == (wchar_t *)1);
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(r, &stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats.live_bytes == live && stats.allocated_bytes == total && stats.peak_bytes == peak);
    xr_compile_resources_release(r);
    if (status == XR_OS_IO_OK) { CHECK(!wcsncmp(out, L"\\\\?\\", 4)); xr_compile_resources_free(out); }
    CHECK(!live); return stats;
}
static void matrix(const char *path) {
    attempts = edge_count = 0; record_edges = true;
    XrCompileResourceStats stats = convert(path, unlimited, XR_OS_IO_OK);
    record_edges = false; size_t mallocs = attempts, count = edge_count;
    uint64_t *points = physical_malloc(count * sizeof(*points)); CHECK(points);
    memcpy(points, edges, count * sizeof(*points));
    for (size_t i = 0; i < mallocs; ++i) {
        fail_at = i; attempts = 0; convert(path, unlimited, XR_OS_IO_OUT_OF_MEMORY);
    }
    fail_at = SIZE_MAX;
    XrCompileResourceLimits exact = {stats.allocated_bytes, stats.peak_bytes, stats.work};
    convert(path, exact, XR_OS_IO_OK);
    for (unsigned i = 0; i < 3; ++i) {
        XrCompileResourceLimits limits = unlimited;
        if (i == 0) limits.allocated_bytes = stats.allocated_bytes - 1;
        if (i == 1) limits.live_bytes = stats.peak_bytes - 1;
        if (i == 2) limits.work = stats.work - 1;
        convert(path, limits, XR_OS_IO_BUDGET);
    }
    for (size_t i = 0; i < count; ++i) {
        XrCompileResourceLimits limits = unlimited; limits.work = points[i] - 1;
        convert(path, limits, XR_OS_IO_BUDGET);
    }
    physical_free(points);
    XrCompileResourceLimits shared_limits = unlimited; shared_limits.work = 2 * stats.work - 2;
    XrCompileResources *r = ledger(&shared_limits); XrOsIoPolicy policy = xr_compile_io_policy(r);
    wchar_t *first = NULL, *second = (wchar_t *)2;
    CHECK(xr_win_utf8_path_owned(&policy, path, &first) == XR_OS_IO_OK);
    CHECK(xr_win_utf8_path_owned(&policy, path, &second) == XR_OS_IO_BUDGET && second == (wchar_t *)2);
    xr_compile_resources_free(first); xr_compile_resources_release(r); CHECK(!live);
    printf("long locator malloc=%zu work_edges=%zu bytes=%llu peak=%llu work=%llu; all OOM/cutoffs/exact/minus1/cumulative/producer lifetime PASS\n",
        mallocs,count,(unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
}
static void semantics(const char *file, const char *directory) {
    XrCompileResources *r = ledger(&unlimited); XrOsIoPolicy policy = xr_compile_io_policy(r);
    wchar_t *wide = NULL;
    CHECK(xr_win_utf8_path_owned(&policy,"part/../name. ",&wide) == XR_OS_IO_OK);
    CHECK(!wcscmp(wide,L"part/../name. ")); xr_compile_resources_free(wide); wide = NULL;
    CHECK(xr_win_utf8_text_owned(&policy,file,&wide) == XR_OS_IO_OK && wcsncmp(wide,L"\\\\?\\",4));
    xr_compile_resources_free(wide); wide = NULL;
    char value[4096];
    int n = snprintf(value,sizeof(value),"%s/../%s/source.xr",directory,strrchr(directory,'\\')+1);
    CHECK(n > 0 && (size_t)n < sizeof(value));
    CHECK(xr_win_utf8_path_owned(&policy,value,&wide) == XR_OS_IO_OK);
    HANDLE h = CreateFileW(wide,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(h != INVALID_HANDLE_VALUE && CloseHandle(h)); xr_compile_resources_free(wide); wide = NULL;
    const char *bad[] = {"NUL.txt","COM1","LPT\xC2\xB9","bad.\\file","bad \\file","tail.","tail "};
    for (unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
        n=snprintf(value,sizeof(value),"%s\\%s",directory,bad[i]);CHECK(n>0&&(size_t)n<sizeof(value));
        wide=(wchar_t *)1;CHECK(xr_win_utf8_path_owned(&policy,value,&wide)==XR_OS_IO_BAD_ARGUMENT&&wide==(wchar_t *)1);
    }
    n=snprintf(value,sizeof(value),"\\\\?\\%s",file);CHECK(n>0&&(size_t)n<sizeof(value));wide=NULL;
    CHECK(xr_win_utf8_path_owned(&policy,value,&wide)==XR_OS_IO_OK && !wcsncmp(wide,L"\\\\?\\",4));
    CHECK(wcsstr(wide+4,L"\\\\?\\")==NULL);xr_compile_resources_free(wide);
    n=snprintf(value,sizeof(value),"\\\\.\\%s",file);CHECK(n>0&&(size_t)n<sizeof(value));wide=NULL;
    CHECK(xr_win_utf8_path_owned(&policy,value,&wide)==XR_OS_IO_OK&&!wcsncmp(wide,L"\\\\.\\",4));xr_compile_resources_free(wide);
    n=snprintf(value,sizeof(value),"\\\\server\\share\\%s",file+3);CHECK(n>0&&(size_t)n<sizeof(value));wide=NULL;
    CHECK(xr_win_utf8_path_owned(&policy,value,&wide)==XR_OS_IO_OK&&!wcsncmp(wide,L"\\\\?\\UNC\\server\\share\\",21));
    xr_compile_resources_free(wide);
    xr_compile_resources_release(r);CHECK(!live);
    puts("short/text unchanged; long slash/dot normalized; reserved components reject; explicit prefix unchanged PASS");
}
static void forms_and_limit(const char *root,const char *file) {
    XrCompileResources *r=ledger(&unlimited);XrOsIoPolicy policy=xr_compile_io_policy(r);
    const char *relative=file+strlen(root)+1;char drive[4096];
    int n=snprintf(drive,sizeof(drive),"%c:%s",file[0],relative);CHECK(n>0&&(size_t)n<sizeof(drive));
    const char *forms[]={relative,file+2,drive};
    for(unsigned i=0;i<3;++i){XrFsStat stat={0};CHECK(xr_os_io_stat(&policy,forms[i],&stat)==XR_OS_IO_OK&&stat.kind==XR_FS_FILE&&stat.size==3);}
    char *huge=physical_malloc(32769);CHECK(huge);huge[0]=file[0];huge[1]=':';huge[2]='\\';
    for(size_t i=3;i<32767;++i)huge[i]=i%2?'a':'\\';huge[32767]=0;
    wchar_t *sentinel=(wchar_t *)1;
    CHECK(xr_win_utf8_path_owned(&policy,huge,&sentinel)==XR_OS_IO_BUDGET&&sentinel==(wchar_t *)1);
    physical_free(huge);xr_compile_resources_release(r);CHECK(!live);
    puts("relative/rooted/drive-relative long locators find actual bytes; prefix-inclusive limit rejects PASS");
}
static void integration(const char *root, const char *file, const char *directory) {
    XrCompileResources *r=ledger(&unlimited);XrOsIoPolicy policy=xr_compile_io_policy(r);
    XrFsStat stat={0};CHECK(xr_os_io_stat(&policy,file,&stat)==XR_OS_IO_OK&&stat.kind==XR_FS_FILE&&stat.size==3);
    CHECK(xr_file_probe_owned(&policy,file,false)==XR_OS_IO_OK);
    uint8_t *bytes=NULL;size_t size=0;
    CHECK(xr_os_io_read_regular_file(&policy,file,3,&bytes,&size)==XR_OS_IO_OK&&size==3&&!memcmp(bytes,"abc",3));
    xr_compile_resources_free(bytes);
    XrDirIter *iterator=NULL;CHECK(xr_os_io_dir_open(&policy,directory,&iterator)==XR_OS_IO_OK);
    XrDirEntry entry;bool found=false;
    for (;;) { XrOsIoStatus s=xr_os_io_dir_next(iterator,&entry);if(s==XR_OS_IO_END)break;CHECK(s==XR_OS_IO_OK);if(!strcmp(entry.name,"source.xr"))found=true; }
    CHECK(found);xr_os_io_dir_close(iterator);
    char *canonical=NULL;CHECK(xr_realpath_owned(&policy,file,&canonical)==XR_OS_IO_OK);
    CHECK(!strcmp(canonical,file)&&strncmp(canonical,"\\\\?\\",4));xr_compile_resources_free(canonical);
    char logical[4096];size_t n=strlen(root)+1;CHECK(strlen(file)>=n&&strlen(file+n)<sizeof(logical));strcpy(logical,file+n);
    for(char *p=logical;*p;++p)if(*p=='\\')*p='/';
    XrFileBytes contents={0};CHECK(xr_os_io_read_under_root(&policy,root,logical,3,&contents)==XR_FILE_READ_OK&&contents.size==3);
    XrFileBytes nested={0};CHECK(xr_os_io_read_under_root(&policy,directory,"source.xr",3,&nested)==XR_FILE_READ_OK);
    CHECK(nested.size==3&&!memcmp(nested.data,"abc",3));xr_compile_resources_free(nested.data);
    XrFileBytes forbidden={(char *)1,99};
    CHECK(xr_os_io_read_under_root(&policy,directory,"./source.xr",3,&forbidden)==XR_FILE_READ_FORBIDDEN);
    CHECK(forbidden.data==(char *)1&&forbidden.size==99);
    xr_compile_resources_release(r);CHECK(!memcmp(contents.data,"abc",3));xr_compile_resources_free(contents.data);CHECK(!live);
    puts("real typed stat/probe/dir/read/physical-root read and public canonical spelling PASS");
}
int wmain(int argc,wchar_t **argv) {
    CHECK(argc==5);XrWinPathStatus status;char **args=xr_win_utf16_arguments(argc,argv,&status);CHECK(args&&status==XR_WIN_PATH_OK);
    DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    integration(args[1],args[2],args[3]);integration(args[1],args[4],args[3]);
    semantics(args[2],args[3]);forms_and_limit(args[1],args[2]);matrix(args[2]);
    convert(args[2]+strlen(args[1])+1,unlimited,XR_OS_IO_OK);
#ifndef WINDOWS_PATH_PRODUCTION
    const DWORD failures[]={ERROR_ACCESS_DENIED,ERROR_NOT_ENOUGH_MEMORY,ERROR_OUTOFMEMORY};
    for(unsigned i=0;i<3;++i){full_error=failures[i];convert(args[2],unlimited,i?XR_OS_IO_OUT_OF_MEMORY:XR_OS_IO_IO);}
    full_error=0;full_overflow=true;convert(args[2],unlimited,XR_OS_IO_BUDGET);full_overflow=false;
    XrCompileResources *r=ledger(&unlimited);XrOsIoPolicy policy=xr_compile_io_policy(r);wchar_t *slot=NULL;
    CHECK(xr_win_utf8_text_owned(&policy,args[2],&slot)==XR_OS_IO_OK);wchar_t *original=slot;
    full_error=ERROR_ACCESS_DENIED;
    CHECK(xr_win_path_locator_owned(&policy,&slot,wcslen(slot))==XR_OS_IO_IO&&slot==original);
    full_error=0;xr_compile_resources_free(slot);xr_compile_resources_release(r);CHECK(!live);
    printf("GetFullPathNameW actual/error/overflow calls=%zu PASS\n",full_calls);
#endif
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before&&!live);
    xr_win_utf8_arguments_free(argc,args);puts("physical tracked heap/handles zero PASS");return 0;
}
