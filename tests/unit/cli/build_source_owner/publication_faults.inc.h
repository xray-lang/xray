/* Real ledger and Win32 failures; no substitute publisher or filesystem. */
#include "base/xmalloc.h"
#include <bcrypt.h>
static size_t allocation_attempts, fail_allocation=SIZE_MAX, live_allocations;
static size_t io_attempts, fail_io=SIZE_MAX;
static DWORD injected_error=ERROR_ACCESS_DENIED;
static bool record_work, collision, zero_write, short_write, fail_disposition, force_flush_failure;
static size_t close_attempts, fail_close=SIZE_MAX;
static bool permanent_close;
static uint64_t work_points[32768];
static size_t work_count;
static void *publication_test_malloc(size_t bytes) {
    if(allocation_attempts++==fail_allocation)return NULL;
    void *memory=xr_malloc(bytes);if(memory)++live_allocations;return memory;
}
static void publication_test_free(void *memory) {
    if(memory){CHECK(live_allocations);--live_allocations;}xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc publication_test_malloc
#define xr_free publication_test_free
#define xr_compile_resources_work publication_real_work
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work
#undef xr_malloc
#undef xr_free
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *owner,uint64_t units) {
    if(record_work && units){CHECK(work_count<32768);work_points[work_count++]=owner->stats.work+units;}
    return publication_real_work(owner,units);
}
static bool publication_fault(void) {
    if(io_attempts++!=fail_io)return false;
    SetLastError(injected_error);return true;
}
static DWORD WINAPI publication_fullpath(LPCWSTR name,DWORD count,LPWSTR out,LPWSTR *part) {
    if(publication_fault())return 0;
    return GetFullPathNameW(name,count,out,part);
}
static HANDLE WINAPI publication_createfile(LPCWSTR path,DWORD access,DWORD share,
    LPSECURITY_ATTRIBUTES security,DWORD creation,DWORD flags,HANDLE template_handle) {
    if(publication_fault())return INVALID_HANDLE_VALUE;
    if(collision && creation==CREATE_NEW){SetLastError(ERROR_FILE_EXISTS);return INVALID_HANDLE_VALUE;}
    return CreateFileW(path,access,share,security,creation,flags,template_handle);
}
static BOOL WINAPI publication_volume(HANDLE handle,LPWSTR name,DWORD name_size,LPDWORD serial,
    LPDWORD maximum,LPDWORD flags,LPWSTR filesystem,DWORD filesystem_size) {
    if(publication_fault())return FALSE;
    return GetVolumeInformationByHandleW(handle,name,name_size,serial,maximum,flags,filesystem,filesystem_size);
}
static DWORD WINAPI publication_attributes(LPCWSTR name) {
    if(publication_fault())return INVALID_FILE_ATTRIBUTES;
    return GetFileAttributesW(name);
}
static NTSTATUS WINAPI publication_random(BCRYPT_ALG_HANDLE algorithm,PUCHAR buffer,ULONG length,ULONG flags) {
    if(publication_fault())return injected_error==ERROR_ACCESS_DENIED?(NTSTATUS)0xc0000022:(NTSTATUS)0xc0000017;
    return BCryptGenRandom(algorithm,buffer,length,flags);
}
static BOOL WINAPI publication_writefile(HANDLE file,LPCVOID bytes,DWORD length,LPDWORD count,LPOVERLAPPED overlapped) {
    if(publication_fault())return FALSE;
    if(zero_write){*count=0;return TRUE;}
    return WriteFile(file,bytes,short_write && length>1?1:length,count,overlapped);
}
static BOOL WINAPI publication_flush(HANDLE file) {
    if(force_flush_failure){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    if(publication_fault())return FALSE;
    return FlushFileBuffers(file);
}
static BOOL WINAPI publication_setinfo(HANDLE file,FILE_INFO_BY_HANDLE_CLASS kind,LPVOID value,DWORD size) {
    if(kind==FileRenameInfo && publication_fault())return FALSE;
    if(kind==FileDispositionInfo && fail_disposition){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return SetFileInformationByHandle(file,kind,value,size);
}
static BOOL WINAPI publication_closehandle(HANDLE file) {
    if(permanent_close || close_attempts++==fail_close){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return CloseHandle(file);
}
#define GetFullPathNameW publication_fullpath
#define CreateFileW publication_createfile
#define GetVolumeInformationByHandleW publication_volume
#define GetFileAttributesW publication_attributes
#define BCryptGenRandom publication_random
#define WriteFile publication_writefile
#define FlushFileBuffers publication_flush
#define SetFileInformationByHandle publication_setinfo
#define CloseHandle publication_closehandle
#include "app/toolchain/xtc_xir_publication.c"
#undef GetFullPathNameW
#undef CreateFileW
#undef GetVolumeInformationByHandleW
#undef GetFileAttributesW
#undef BCryptGenRandom
#undef WriteFile
#undef FlushFileBuffers
#undef SetFileInformationByHandle
#undef CloseHandle
