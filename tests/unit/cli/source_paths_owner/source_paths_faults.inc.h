/* Physical allocator and submitted-work observations at the real ledger. */
#ifndef SOURCE_PATHS_FAULTS_H
#define SOURCE_PATHS_FAULTS_H
#include "base/xmalloc.h"
static void *physical_malloc(size_t n) { return xr_malloc(n); }
static void physical_free(void *p) { xr_free(p); }
typedef struct TestBlock { void *pointer; size_t bytes; } TestBlock;
static TestBlock blocks[512];
static size_t attempts, fail_at = SIZE_MAX, live, total, peak;
static uint64_t edges[65536];
static size_t edge_count;
static bool record_edges;
static void *observed_malloc(size_t n) {
    if (attempts++ == fail_at) return NULL;
    void *p = physical_malloc(n); if (!p) return NULL;
    for (size_t i = 0; i < 512; ++i) if (!blocks[i].pointer) {
        blocks[i] = (TestBlock){p, n}; live += n; total += n;
        if (live > peak) peak = live;
        return p;
    }
    CHECK(false); return NULL;
}
static void observed_free(void *p) {
    if (!p) return;
    for (size_t i = 0; i < 512; ++i) if (blocks[i].pointer == p) {
        live -= blocks[i].bytes; blocks[i] = (TestBlock){0}; physical_free(p); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc observed_malloc
#define xr_free observed_free
#define xr_compile_resources_work paths_real_work
#define xr_compile_resources_alloc paths_real_alloc
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work
#undef xr_compile_resources_alloc
#undef xr_malloc
#undef xr_free
#define xr_malloc physical_malloc
#define xr_free physical_free
static XrCompileResourceStatus record_status(XrCompileResources *r, XrCompileResourceStatus s) {
    if (record_edges && s == XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats stats; CHECK(xr_compile_resources_stats(r, &stats) == XR_COMPILE_RESOURCE_OK);
        if (!edge_count || edges[edge_count - 1] != stats.work) {
            CHECK(edge_count < 65536); edges[edge_count++] = stats.work;
        }
    }
    return s;
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r, uint64_t n) {
    return record_status(r, paths_real_work(r, n));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_alloc(XrCompileResources *r, size_t n, void **out) {
    return record_status(r, paths_real_alloc(r, n, out));
}
#ifndef SOURCE_PATHS_PRODUCTION
static size_t io_attempts, io_fail_at = SIZE_MAX;
static DWORD io_error = ERROR_ACCESS_DENIED;
static bool environment_grows, module_truncates;
static bool reject_io(void) {
    if (io_attempts++ != io_fail_at) return false;
    SetLastError(io_error); return true;
}
static DWORD observed_module(HMODULE h, LPWSTR s, DWORD n) {
    if (reject_io()) return 0;
    if (module_truncates) { if (n) s[0] = L'x'; return n; }
    return GetModuleFileNameW(h, s, n);
}
static DWORD observed_environment(LPCWSTR k, LPWSTR s, DWORD n) {
    if (reject_io()) return 0;
    if (environment_grows && s && n) { s[0] = L'x'; return n + 1; }
    return GetEnvironmentVariableW(k, s, n);
}
static int observed_multi(UINT cp, DWORD flags, LPCCH s, int n, LPWSTR out, int capacity) {
    return reject_io() ? 0 : MultiByteToWideChar(cp, flags, s, n, out, capacity);
}
static int observed_wide(UINT cp, DWORD flags, LPCWCH s, int n, LPSTR out, int capacity, LPCCH d, LPBOOL used) {
    return reject_io() ? 0 : WideCharToMultiByte(cp, flags, s, n, out, capacity, d, used);
}
#define GetModuleFileNameW observed_module
#define GetEnvironmentVariableW observed_environment
#define MultiByteToWideChar observed_multi
#define WideCharToMultiByte observed_wide
#include "os/win/proc_self_win.c"
#undef GetModuleFileNameW
#undef GetEnvironmentVariableW
#undef MultiByteToWideChar
#undef WideCharToMultiByte
#endif
#endif // SOURCE_PATHS_FAULTS_H
