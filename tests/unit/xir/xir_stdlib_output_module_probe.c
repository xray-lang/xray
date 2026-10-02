/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_stdlib_output_module_probe.c - Actual resolver, identity and graph allocations
 */
#include "base/xmalloc.h"
#include "xir_stdlib_output_module_probe.h"
#include <stdint.h>
#include <stdlib.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#endif
size_t module_attempts, module_fail_at = SIZE_MAX, module_live, module_bytes;
bool module_injecting;
size_t module_os_attempts[2], module_os_fail_at[2] = {SIZE_MAX, SIZE_MAX};
unsigned long module_os_error;
bool module_attributes_override;
unsigned long module_attributes;
#ifdef XR_OS_WINDOWS
static bool module_os_reject(unsigned kind) {
    if (!module_injecting || module_os_attempts[kind]++ != module_os_fail_at[kind]) return false;
    SetLastError(module_os_error); return true;
}
static BOOL module_get_attributes(const wchar_t *path, GET_FILEEX_INFO_LEVELS level, void *out) {
    if (module_os_reject(0)) return FALSE;
    if (module_injecting && module_attributes_override) {
        WIN32_FILE_ATTRIBUTE_DATA *attributes = out;
        memset(attributes, 0, sizeof(*attributes)); attributes->dwFileAttributes = module_attributes;
        return TRUE;
    }
    return GetFileAttributesExW(path, level, out);
}
/* Inject at the actual API invocation, after the production UTF-16 allocation. */
#define GetFileAttributesExW(path, level, out) \
    module_get_attributes(path, level, out)
#define GetFullPathNameW(path, length, out, part) \
    (module_os_reject(1) ? 0UL : GetFullPathNameW(path, length, out, part))
#endif
typedef struct ModuleAllocation { void *pointer; size_t bytes; } ModuleAllocation;
static ModuleAllocation module_allocations[4096];
static size_t module_find(void *p) {
    for (size_t i = 0; i < module_live; ++i) if (module_allocations[i].pointer == p) return i;
    return module_live;
}
static void module_record(void *p, size_t bytes) {
    if (!p || !module_injecting) return;
    if (module_live == 4096 || bytes > SIZE_MAX - module_bytes) abort();
    module_allocations[module_live++] = (ModuleAllocation){p, bytes}; module_bytes += bytes;
}
static bool module_reject(void) {
    return module_injecting && module_attempts++ == module_fail_at;
}
static void *module_malloc(size_t size) {
    if (module_reject()) return NULL;
    void *p = xr_malloc(size); module_record(p, size); return p;
}
static void *module_calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) abort();
    if (module_reject()) return NULL;
    void *p = xr_calloc(count, size); module_record(p, count * size); return p;
}
XR_FUNC void xr_test_stdlib_output_module_forget(void *p) {
    size_t i = module_find(p);
    if (i < module_live) {
        module_bytes -= module_allocations[i].bytes;
        module_allocations[i] = module_allocations[--module_live];
    }
}
static void module_free(void *p) {
    xr_test_stdlib_output_module_forget(p); xr_free(p);
}
static void *module_realloc(void *p, size_t size) {
    if (module_reject()) return NULL;
    size_t i = module_find(p);
    void *next = xr_realloc(p, size);
    if (!next) return NULL;
    if (i < module_live) {
        module_bytes -= module_allocations[i].bytes;
        module_allocations[i] = module_allocations[--module_live];
    }
    module_record(next, size); return next;
}
static char *module_strdup(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = module_malloc(length);
    if (copy) memcpy(copy, text, length);
    return copy;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc module_malloc
#define xr_calloc module_calloc
#define xr_realloc module_realloc
#define xr_free module_free
#define xr_strdup module_strdup
#include "base/xfileio.c"
#include "module/xmodule_identity.c"
#include "module/xmodule_resolver.c"
#include "module/xmodule_graph.c"
