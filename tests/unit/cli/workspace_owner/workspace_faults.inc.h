/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * workspace_faults.inc.h - Real allocator and workspace OS failure boundaries
 */
#include "base/xmalloc.h"
static size_t runtime_bytes, physical_total, physical_peak;
static bool record_physical;
static void *workspace_physical_malloc(size_t bytes) {
    void *memory = xr_malloc(bytes);
    if (memory && record_physical) {
        physical_total += bytes;
        if (runtime_bytes + bytes > physical_peak) physical_peak = runtime_bytes + bytes;
    }
    return memory;
}
#undef xr_malloc
#define xr_malloc workspace_physical_malloc
#define xr_compile_resources_work workspace_real_work
#define xr_compile_resources_alloc workspace_real_alloc
#define xr_compile_resources_calloc workspace_real_calloc
#define xr_compile_resources_resize workspace_real_resize
#include "../../xir/xir_sdk_resource_test.h"
#undef xr_compile_resources_work
#undef xr_compile_resources_alloc
#undef xr_compile_resources_calloc
#undef xr_compile_resources_resize
static bool record_work;
static uint64_t work_edges[65536];
static size_t work_count;
static XrCompileResourceStatus workspace_record_work(XrCompileResources *r, XrCompileResourceStatus result) {
    (void)sdk_fixture_malloc; (void)sdk_fixture_free;
    if (record_work && result == XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats stats = sdk_stats(r);
        if (!work_count || work_edges[work_count - 1] != stats.work) {
            CHECK(work_count < 65536); work_edges[work_count++] = stats.work;
        }
    }
    return result;
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r, uint64_t units) {
    return workspace_record_work(r, workspace_real_work(r, units));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(XrCompileResources *r, size_t n, void **out) {
    return workspace_record_work(r, workspace_real_alloc(r, n, out));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(XrCompileResources *r, size_t n, size_t size, void **out) {
    return workspace_record_work(r, workspace_real_calloc(r, n, size, out));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_resize(XrCompileResources *r, void **p, size_t n) {
    return workspace_record_work(r, workspace_real_resize(r, p, n));
}
#ifndef WORKSPACE_PRODUCTION
static size_t io_count, io_fail_at = SIZE_MAX;
static DWORD io_error = ERROR_ACCESS_DENIED;
static bool fail_io(void);
static bool fixed_nonce;
static bool stop_before_input;
static bool corrupt_identity;
static HANDLE owner_handles[128];
static size_t owner_handle_count;
static HANDLE workspace_record_handle(HANDLE handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
        for (size_t i = 0; i < 128; ++i) if (!owner_handles[i]) {
            owner_handles[i] = handle; ++owner_handle_count; return handle;
        }
        CHECK(false);
    }
    return handle;
}
static BOOL workspace_close_handle(HANDLE handle) {
    if (fail_io()) return FALSE;
    BOOL okay = CloseHandle(handle);
    if (okay) {
        bool found = false;
        for (size_t i = 0; i < 128; ++i) if (owner_handles[i] == handle) {
            owner_handles[i] = NULL; --owner_handle_count; found = true; break;
        }
        CHECK(found);
    }
    return okay;
}
static bool fail_io(void) {
    if (io_count++ != io_fail_at) return false;
    SetLastError(io_error); return true;
}
static NTSTATUS failure_ntstatus(void) {
    return (NTSTATUS)(io_error == ERROR_NOT_ENOUGH_MEMORY ? 0xc0000017UL : 0xc0000022UL);
}
static NTSTATUS workspace_test_nt(PHANDLE out, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
    PIO_STATUS_BLOCK result, PLARGE_INTEGER size, ULONG attrs, ULONG share, ULONG disposition,
    ULONG options, PVOID ea, ULONG ea_size) {
    if (fail_io()) return failure_ntstatus();
    if (stop_before_input && disposition == FILE_CREATE && attributes->ObjectName->Length == 10 &&
        !memcmp(attributes->ObjectName->Buffer, L"input", 10)) return (NTSTATUS)0xc0000022UL;
    NTSTATUS status = NtCreateFile(out, access, attributes, result, size, attrs, share, disposition, options, ea, ea_size);
    if (status >= 0) workspace_record_handle(*out);
    return status;
}
static NTSTATUS workspace_random(BCRYPT_ALG_HANDLE algorithm, PUCHAR bytes, ULONG count, ULONG flags) {
    if (fail_io()) return failure_ntstatus();
    NTSTATUS status = BCryptGenRandom(algorithm, bytes, count, flags);
    if (status >= 0 && fixed_nonce) memset(bytes, 0, count);
    return status;
}
static BOOL workspace_information(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID output, DWORD bytes) {
    if (fail_io()) return FALSE;
    BOOL okay = GetFileInformationByHandleEx(handle, kind, output, bytes);
    if (okay && corrupt_identity && kind == FileIdInfo) {
        ((FILE_ID_INFO *)output)->FileId.Identifier[15] ^= 1; corrupt_identity = false;
    }
    return okay;
}
#define NtCreateFile workspace_test_nt
#define BCryptGenRandom workspace_random
#define CreateFileW(...) (fail_io() ? INVALID_HANDLE_VALUE : workspace_record_handle(CreateFileW(__VA_ARGS__)))
#define GetFileInformationByHandleEx workspace_information
#define GetVolumeInformationByHandleW(...) (fail_io() ? FALSE : GetVolumeInformationByHandleW(__VA_ARGS__))
#define SetFileInformationByHandle(...) (fail_io() ? FALSE : SetFileInformationByHandle(__VA_ARGS__))
#define GetFileType(...) (fail_io() ? FILE_TYPE_UNKNOWN : GetFileType(__VA_ARGS__))
#define CloseHandle workspace_close_handle
#include "app/toolchain/xtc_xir_workspace.c"
#undef NtCreateFile
#undef BCryptGenRandom
#undef CreateFileW
#undef GetFileInformationByHandleEx
#undef GetVolumeInformationByHandleW
#undef SetFileInformationByHandle
#undef GetFileType
#undef CloseHandle
#endif
