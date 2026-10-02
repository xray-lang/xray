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
size_t module_attempts, module_fail_at = SIZE_MAX, module_live, module_bytes;
bool module_injecting;
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
