/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * stdlib_cache.c - Per-isolate stdlib cache: create / get / free
 *
 * The cache pointer lives as an opaque `void *` on XrVMRuntime so that
 * the core header never needs to include stdlib types. This translation
 * unit is the only place that casts between the opaque pointer and the
 * concrete XrStdlibCache struct, keeping the coupling local.
 */

#include "stdlib_cache.h"

#include <string.h>

#include "../src/base/xmalloc.h"
#include "../src/runtime/class/xinstance.h"
#include "../src/runtime/class/xenum.h"
#include "../src/runtime/xisolate_internal.h"
#include "../src/stdlib/xstdlib_defs_generated.h"

typedef struct XrStdlibNativeEnumCacheEntry {
    const XrStdlibEnumDefEntry *decl;
    XrEnumType *type;
} XrStdlibNativeEnumCacheEntry;

typedef struct XrStdlibNativeRecordCacheEntry {
    const XrStdlibRecordDefEntry *decl;
    XrClass *cls;
} XrStdlibNativeRecordCacheEntry;

static const XrStdlibRecordDefEntry *stdlib_record_decl_find(const char *module, const char *name) {
    if (!module || !name)
        return NULL;
    for (uint32_t i = 0; i < XR_STDLIB_RECORD_DEF_ENTRY_COUNT; i++) {
        const XrStdlibRecordDefEntry *decl = &xr_stdlib_record_def_entries[i];
        if (strcmp(decl->module, module) == 0 && strcmp(decl->name, name) == 0)
            return decl;
    }
    return NULL;
}

static XrClass *stdlib_record_class_build(XrVMRuntime *isolate,
                                          const XrStdlibRecordDefEntry *decl) {
    uint32_t count = decl ? decl->field_count : 0;
    if (!isolate || !decl || count == 0 || !decl->fields)
        return NULL;
    const char **names = (const char **) xr_calloc(count, sizeof(*names));
    if (!names)
        return NULL;
    for (uint32_t i = 0; i < count; i++)
        names[i] = decl->fields[i].name;
    XrClass *cls =
        xr_class_build_record_chain(isolate, names, NULL, (int) count, NULL, NULL, NULL,
                                    decl->sealed);
    xr_free(names);
    return cls;
}

static const XrStdlibEnumDefEntry *stdlib_enum_decl_find(const char *module, const char *name) {
    if (!module || !name)
        return NULL;
    for (uint32_t i = 0; i < XR_STDLIB_ENUM_DEF_ENTRY_COUNT; i++) {
        const XrStdlibEnumDefEntry *decl = &xr_stdlib_enum_def_entries[i];
        if (strcmp(decl->module, module) == 0 && strcmp(decl->name, name) == 0)
            return decl;
    }
    return NULL;
}

static XrEnumType *stdlib_enum_type_build(XrVMRuntime *isolate, const XrStdlibEnumDefEntry *decl) {
    int count = decl ? (int) decl->variant_count : 0;
    if (!isolate || !decl || count <= 0 || !decl->variants)
        return NULL;
    char **names = (char **) xr_calloc((size_t) count, sizeof(*names));
    int *payload_counts = (int *) xr_calloc((size_t) count, sizeof(*payload_counts));
    if (!names || !payload_counts) {
        xr_free(names);
        xr_free(payload_counts);
        return NULL;
    }
    bool has_payloads = false;
    for (int i = 0; i < count; i++) {
        names[i] = (char *) decl->variants[i].name;
        payload_counts[i] = (int) decl->variants[i].payload_count;
        has_payloads = has_payloads || payload_counts[i] > 0;
    }
    XrEnumType *type = xr_enum_type_new(isolate, decl->module, decl->name, names, count);
    if (type && has_payloads && !xr_enum_type_set_adt_payloads(type, payload_counts, count))
        type = NULL;
    xr_free(names);
    xr_free(payload_counts);
    if (!type)
        return NULL;

    if (type->layout && decl->layout_id != 0) {
        type->layout->layout_id = decl->layout_id;
        for (uint32_t i = 0; i < type->member_count; i++) {
            if (type->members[i].ctor)
                type->members[i].ctor->layout_id = decl->layout_id;
        }
    }
    return type;
}

XR_FUNC XrStdlibCache *xr_stdlib_cache_get(XrVMRuntime *isolate) {
    XrStdlibCache *c = (XrStdlibCache *) isolate->stdlib_cache;
    if (c)
        return c;
    c = (XrStdlibCache *) xr_malloc(sizeof(XrStdlibCache));
    if (!c) {
        /* Match xmalloc OOM policy used by the rest of stdlib. */
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    isolate->stdlib_cache = c;
    return c;
}

XR_FUNC XrEnumType *xr_stdlib_enum_type_get(XrVMRuntime *isolate, const char *module,
                                            const char *name) {
    const XrStdlibEnumDefEntry *decl = stdlib_enum_decl_find(module, name);
    XrStdlibCache *cache = isolate ? xr_stdlib_cache_get(isolate) : NULL;
    if (!decl || !cache)
        return NULL;
    XrStdlibNativeEnumCacheEntry *entries =
        (XrStdlibNativeEnumCacheEntry *) cache->native_enum_cache;
    for (size_t i = 0; i < cache->native_enum_count; i++) {
        if (entries[i].decl == decl)
            return entries[i].type;
    }

    XrEnumType *type = stdlib_enum_type_build(isolate, decl);
    if (!type)
        return NULL;
    if (cache->native_enum_count == cache->native_enum_capacity) {
        size_t next_capacity = cache->native_enum_capacity ? cache->native_enum_capacity * 2 : 4;
        XrStdlibNativeEnumCacheEntry *next =
            (XrStdlibNativeEnumCacheEntry *) xr_realloc(entries, next_capacity * sizeof(*next));
        if (!next)
            return NULL;
        entries = next;
        cache->native_enum_cache = entries;
        cache->native_enum_capacity = next_capacity;
    }
    entries[cache->native_enum_count++] =
        (XrStdlibNativeEnumCacheEntry) {.decl = decl, .type = type};
    return type;
}

XR_FUNC const char *xr_stdlib_enum_type_module(XrVMRuntime *isolate, const XrEnumType *type) {
    XrStdlibCache *cache = isolate ? (XrStdlibCache *) isolate->stdlib_cache : NULL;
    if (!cache || !type)
        return NULL;
    XrStdlibNativeEnumCacheEntry *entries =
        (XrStdlibNativeEnumCacheEntry *) cache->native_enum_cache;
    for (size_t i = 0; i < cache->native_enum_count; i++) {
        if (entries[i].type == type)
            return entries[i].decl->module;
    }
    return NULL;
}

XR_FUNC XrClass *xr_stdlib_record_class_get(XrVMRuntime *isolate, const char *module,
                                            const char *name) {
    const XrStdlibRecordDefEntry *decl = stdlib_record_decl_find(module, name);
    XrStdlibCache *cache = isolate ? xr_stdlib_cache_get(isolate) : NULL;
    if (!decl || !cache)
        return NULL;
    XrStdlibNativeRecordCacheEntry *entries =
        (XrStdlibNativeRecordCacheEntry *) cache->native_record_cache;
    for (size_t i = 0; i < cache->native_record_count; i++) {
        if (entries[i].decl == decl)
            return entries[i].cls;
    }

    XrClass *cls = stdlib_record_class_build(isolate, decl);
    if (!cls)
        return NULL;
    if (cache->native_record_count == cache->native_record_capacity) {
        size_t next_capacity =
            cache->native_record_capacity ? cache->native_record_capacity * 2 : 4;
        XrStdlibNativeRecordCacheEntry *next =
            (XrStdlibNativeRecordCacheEntry *) xr_realloc(entries, next_capacity * sizeof(*next));
        if (!next)
            return NULL;
        entries = next;
        cache->native_record_cache = entries;
        cache->native_record_capacity = next_capacity;
    }
    entries[cache->native_record_count++] =
        (XrStdlibNativeRecordCacheEntry) {.decl = decl, .cls = cls};
    return cls;
}

XR_FUNC void xr_stdlib_cache_free(XrVMRuntime *isolate) {
    if (!isolate || !isolate->stdlib_cache)
        return;
    XrStdlibCache *c = (XrStdlibCache *) isolate->stdlib_cache;

    /* Tear down per-isolate log state (async thread, mutex, logger). */
    if (c->log_state_cleanup && c->log_state) {
        c->log_state_cleanup(c->log_state);
    }

    xr_free(c->native_record_cache);
    xr_free(c->native_enum_cache);

    /* Shapes and interned strings are GC-managed; freeing the
     * container is sufficient. */
    xr_free(c);
    isolate->stdlib_cache = NULL;
}
