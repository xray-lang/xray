/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_compile_internal.h - Shared module work and owned storage helpers
 */
#ifndef XMODULE_COMPILE_INTERNAL_H
#define XMODULE_COMPILE_INTERNAL_H
#include "xmodule_identity_internal.h"
#include "../base/xio_policy.h"
#include <stdarg.h>
#include <string.h>
typedef struct ModuleWork { XrCompileResources *resources; XrModuleStatus status; } ModuleWork;
static inline bool module_status(ModuleWork *work, XrModuleStatus status) {
    if (work->status == XR_MODULE_OK) work->status = status;
    return work->status == XR_MODULE_OK;
}
static inline bool module_io(ModuleWork *work, XrOsIoStatus status) {
    return module_status(work,xr_module_status_from_io(status));
}
static inline void *module_alloc(ModuleWork *work, size_t bytes) {
    void *out = NULL;
    if (work->status == XR_MODULE_OK) module_status(work,
        xr_module_status_from_resource(xr_compile_resources_alloc(work->resources,bytes,&out)));
    return out;
}
static inline void *module_calloc(ModuleWork *work, size_t count, size_t size) {
    void *out = NULL;
    if (work->status == XR_MODULE_OK) module_status(work,
        xr_module_status_from_resource(xr_compile_resources_calloc(work->resources,count,size,&out)));
    return out;
}
static inline bool module_resize(ModuleWork *work, void **pointer, size_t bytes) {
    return work->status == XR_MODULE_OK && module_status(work,
        xr_module_status_from_resource(xr_compile_resources_resize(work->resources,pointer,bytes)));
}
static inline size_t module_length(ModuleWork *work, const char *text) {
    if (!text) { module_status(work,XR_MODULE_INVALID); return 0; }
    size_t i = 0;
    while ((work)->status == XR_MODULE_OK) {
        if (!text[i]) return i;
        if (i == SIZE_MAX-1) { module_status(work,XR_MODULE_BUDGET); break; }
        ++i;
    }
    return 0;
}
static inline bool module_copy(ModuleWork *work, void *dest, const void *source, size_t length) {
    if (!((work)->status == XR_MODULE_OK)) return false;
    if (length) memcpy(dest,source,length); return true;
}
static inline char *module_dup(ModuleWork *work, const char *text) {
    if (!text) return NULL;
    size_t length = module_length(work,text);
    char *out = module_alloc(work,length+1);
    if (out && !module_copy(work,out,text,length+1)) { xr_compile_resources_free(out); out = NULL; }
    return out;
}
static inline bool module_equal(ModuleWork *work, const char *a, const char *b) {
    if (!a || !b) return a == b;
    for (size_t i = 0; (work)->status == XR_MODULE_OK; ++i) {
        char first = a[i], second = b[i];
        if (first != second) return false;
        if (!first) return true;
    }
    return false;
}
static inline bool module_prefix(ModuleWork *work, const char *text, const char *prefix) {
    if (!text || !prefix) return false;
    for (size_t i = 0; (work)->status == XR_MODULE_OK; ++i) {
        char expected = prefix[i];
        if (!expected) return true;
        if (!((work)->status == XR_MODULE_OK) || text[i] != expected) return false;
    }
    return false;
}
static inline const char *module_find_char(ModuleWork *work, const char *text, char character) {
    for (size_t i = 0; (work)->status == XR_MODULE_OK; ++i) {
        char current = text[i];
        if (current == character) return text+i;
        if (!current) return NULL;
    }
    return NULL;
}
static inline bool module_authority(ModuleWork *work, const XrModuleIdentityAuthority *authority) {
    XrModuleIdentityScan scan = {XR_MODULE_OK};
    bool valid = xr_module_identity_authority_walk(&scan,authority);
    if (!valid && work->status == XR_MODULE_OK) module_status(work,
        scan.status == XR_MODULE_OK ? XR_MODULE_INVALID : scan.status);
    return valid;
}
/* This formatter accepts only literal bytes and %s. Both size and copy passes
 * admit every actual format/argument traversal. It never truncates output. */
static inline size_t module_format_pass(ModuleWork *work, char *out, size_t capacity,
    const char *format, va_list args) {
    size_t n = 0;
    for (size_t i = 0; (work)->status == XR_MODULE_OK; ++i) {
        char c = format[i]; if (!c) break;
        if (c == '%') {
            if (!((work)->status == XR_MODULE_OK)) break;
            if (format[++i] != 's') { module_status(work,XR_MODULE_INVALID); break; }
            const char *text = va_arg(args,const char *);
            if (!text) { module_status(work,XR_MODULE_INVALID); break; }
            for (size_t j = 0; (work)->status == XR_MODULE_OK; ++j) {
                if (!text[j]) break;
                if (n == SIZE_MAX-1 || (out && n+1 >= capacity)) { module_status(work,XR_MODULE_BUDGET); break; }
                if (out) out[n] = text[j]; ++n;
            }
        } else {
            if (n == SIZE_MAX-1 || (out && n+1 >= capacity)) { module_status(work,XR_MODULE_BUDGET); break; }
            if (out) out[n] = c; ++n;
        }
    }
    if (out && ((work)->status == XR_MODULE_OK)) out[n] = 0;
    return n;
}
static inline bool module_format_buffer(ModuleWork *work, char *out, size_t capacity, const char *format, ...) {
    if (!capacity) return module_status(work,XR_MODULE_BUDGET);
    va_list args; va_start(args,format); (void)module_format_pass(work,out,capacity,format,args); va_end(args);
    return work->status == XR_MODULE_OK;
}
static inline char *module_format(ModuleWork *work, const char *format, ...) {
    va_list first,second; va_start(first,format); va_copy(second,first);
    size_t n = module_format_pass(work,NULL,0,format,first); va_end(first);
    char *out = module_alloc(work,n+1);
    if (out) (void)module_format_pass(work,out,n+1,format,second);
    va_end(second);
    if (work->status != XR_MODULE_OK) { xr_compile_resources_free(out); out = NULL; }
    return out;
}
/* Requested owned diagnostics are fallible publication, never best effort.
 * Resource failure preserves the output and cannot be recovered as syntax. */
static inline XrModuleStatus module_error(XrCompileResources *resources, char **out, const char *text) {
    if (!out) return XR_MODULE_OK;
    if (!resources || *out) return XR_MODULE_INVALID;
    ModuleWork work = {resources,XR_MODULE_OK}; char *message = module_dup(&work,text);
    if (message) *out = message;
    return work.status;
}
#endif
