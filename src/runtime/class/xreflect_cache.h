/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_cache.h - Per-class reflection cache
 *
 * KEY CONCEPT:
 *   Created on first reflection access.
 *   Pre-creates all Field/Method wrapper objects.
 *   Subsequent queries return cached objects (50-100x speedup).
 */

#ifndef XREFLECT_CACHE_H
#define XREFLECT_CACHE_H

#include "xclass.h"
#include "../base/xdefs.h"

// Forward declarations via xforward_decl.h

typedef struct XrReflectCache {
    XrGCHeader gc;
    XrClass *owner;
    
    XrValue *field_wrappers;
    int field_count;
    
    XrValue *method_wrappers;
    int method_count;
    
    bool initialized;
} XrReflectCache;

/* ========== Cache Operations ========== */

XR_FUNC XrReflectCache* xr_reflect_cache_create(XrayIsolate *X, XrClass *klass);
XR_FUNC void xr_reflect_cache_free(XrReflectCache *cache);

XR_FUNC XrValue xr_reflect_cache_get_field(XrReflectCache *cache, int field_index);
XR_FUNC XrValue xr_reflect_cache_get_method(XrReflectCache *cache, int method_index);

XR_FUNC XrArray* xr_reflect_cache_get_all_fields(XrayIsolate *X, XrReflectCache *cache);
XR_FUNC XrArray* xr_reflect_cache_get_all_methods(XrayIsolate *X, XrReflectCache *cache);

#endif // XREFLECT_CACHE_H
