/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_cache.c - Per-class reflection cache
 *
 * KEY CONCEPT:
 *   Created eagerly by xr_class_builder_finalize alongside the class
 *   itself; the reflection layer reads wrappers straight out of the
 *   cache arrays. See xreflect_cache.h for the end-to-end rationale.
 */

#include "../../base/xchecks.h"
#include "../gc/xgc.h"
#include "xreflect_cache.h"
#include "xreflect_internal.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../object/xarray.h"

/* ========== Cache Creation ========== */

XrReflectCache *xr_reflect_cache_create(XrayIsolate *X, XrClass *klass) {
    if (!X || !klass)
        return NULL;

    XrReflectCache *cache = (XrReflectCache *) XR_ALLOCATE(XrReflectCache);
    if (!cache)
        return NULL;
    // Defensive zero-init so the `fail` label can safely inspect every
    // pointer field without worrying about whether the allocation was
    // reached yet; xr_reflect_cache_free likewise tolerates NULLs.
    memset(cache, 0, sizeof(*cache));

    xr_gc_header_init_type(&cache->gc, XR_TBLOB);
    cache->owner = klass;
    cache->field_count = klass->field_count;
    cache->method_count = klass->method_count;
    cache->initialized = false;

    // Pre-create Field wrappers on the isolate fixedgc list. The wrappers
    // share the regular XR_TINSTANCE owner protocol so xr_gc_cleanup at
    // isolate teardown frees their bodies; this function is responsible
    // only for the cache-owned XrValue arrays.
    if (klass->field_count > 0) {
        cache->field_wrappers = (XrValue *) xr_malloc(sizeof(XrValue) * klass->field_count);
        if (!cache->field_wrappers)
            goto fail;
        for (int i = 0; i < klass->field_count; i++)
            cache->field_wrappers[i] = xr_null();

        for (int i = 0; i < klass->field_count; i++) {
            FieldWrapper *wrapper = (FieldWrapper *) xr_gc_alloc(
                xr_isolate_get_gc(X), sizeof(FieldWrapper), XR_TINSTANCE);
            if (!wrapper)
                goto fail;
            xr_gc_header_init_type(&wrapper->gc, XR_TINSTANCE);
            wrapper->metadata.owner = klass;
            wrapper->metadata.field_index = i;

            cache->field_wrappers[i] = wrapper_to_value(wrapper);
        }
    }

    // Pre-create Method wrappers on the isolate fixedgc list (same owner
    // model as field wrappers above).
    if (klass->method_count > 0) {
        cache->method_wrappers = (XrValue *) xr_malloc(sizeof(XrValue) * klass->method_count);
        if (!cache->method_wrappers)
            goto fail;
        for (int i = 0; i < klass->method_count; i++)
            cache->method_wrappers[i] = xr_null();

        for (int i = 0; i < klass->method_count; i++) {
            MethodWrapper *wrapper = (MethodWrapper *) xr_gc_alloc(
                xr_isolate_get_gc(X), sizeof(MethodWrapper), XR_TINSTANCE);
            if (!wrapper)
                goto fail;
            xr_gc_header_init_type(&wrapper->gc, XR_TINSTANCE);
            wrapper->metadata.owner = klass;
            wrapper->metadata.method_index = i;

            cache->method_wrappers[i] = wrapper_to_value(wrapper);
        }
    }

    cache->initialized = true;
    return cache;

fail:
    // Single cleanup site: xr_reflect_cache_free tolerates NULL entries
    // and NULL field/method arrays, so handing it the partially-built
    // cache is enough regardless of which step of the construction
    // failed. The individual wrapper bodies live on the isolate fixedgc
    // list and will be released by xr_gc_cleanup at isolate teardown,
    // so this function only releases the cache-owned XrValue arrays.
    xr_reflect_cache_free(cache);
    return NULL;
}

void xr_reflect_cache_free(XrReflectCache *cache) {
    if (!cache)
        return;

    if (cache->field_wrappers) {
        xr_free(cache->field_wrappers);
        cache->field_wrappers = NULL;
    }

    if (cache->method_wrappers) {
        xr_free(cache->method_wrappers);
        cache->method_wrappers = NULL;
    }

    xr_free(cache);
}

/* ========== Cache Access ========== */

XrValue xr_reflect_cache_get_field(XrReflectCache *cache, int field_index) {
    if (!cache || !cache->initialized)
        return xr_null();
    if (field_index < 0 || field_index >= cache->field_count)
        return xr_null();

    return cache->field_wrappers[field_index];
}

XrValue xr_reflect_cache_get_method(XrReflectCache *cache, int method_index) {
    if (!cache || !cache->initialized)
        return xr_null();
    if (method_index < 0 || method_index >= cache->method_count)
        return xr_null();

    return cache->method_wrappers[method_index];
}

XrArray *xr_reflect_cache_get_all_fields(XrayIsolate *X, XrReflectCache *cache) {
    XR_DCHECK(X != NULL, "reflect_cache_get_all_fields: NULL isolate");
    if (!cache || !cache->initialized)
        return NULL;

    XrArray *array = xr_array_new(xr_current_coro(X));
    if (!array)
        return NULL;

    for (int i = 0; i < cache->field_count; i++) {
        xr_array_push(array, cache->field_wrappers[i]);
    }

    return array;
}

XrArray *xr_reflect_cache_get_all_methods(XrayIsolate *X, XrReflectCache *cache) {
    XR_DCHECK(X != NULL, "reflect_cache_get_all_methods: NULL isolate");
    if (!cache || !cache->initialized)
        return NULL;

    XrArray *array = xr_array_new(xr_current_coro(X));
    if (!array)
        return NULL;

    for (int i = 0; i < cache->method_count; i++) {
        xr_array_push(array, cache->method_wrappers[i]);
    }

    return array;
}
