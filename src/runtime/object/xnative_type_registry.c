/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_type_registry.c - Dynamic native type registration implementation
 */

#include "xnative_type_registry.h"
#include "../../base/xchecks.h"
#include "../xisolate_api.h"
#include "../gc/xgc.h"
#include <stdlib.h>
#include <string.h>
#include "../../base/xmalloc.h"

// Type registry structure
typedef struct XrNativeTypeRegistry {
    XrNativeTypeInfo **types;
    uint16_t capacity;
    uint16_t count;
    uint16_t next_type_id;
} XrNativeTypeRegistry;

// Get type registry from Isolate
static inline XrNativeTypeRegistry* get_registry(XrayIsolate *isolate) {
    return (XrNativeTypeRegistry*)xr_isolate_get_native_type_registry(isolate);
}

void xr_native_type_registry_init(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "native_type_registry_init: NULL isolate");
    XrNativeTypeRegistry *registry = (XrNativeTypeRegistry*)xr_malloc(sizeof(XrNativeTypeRegistry));
    
    registry->capacity = 64;
    registry->count = 0;
    registry->next_type_id = XR_TYPE_NATIVE_START;
    registry->types = (XrNativeTypeInfo**)xr_calloc(registry->capacity, sizeof(XrNativeTypeInfo*));
    
    xr_isolate_set_native_type_registry(isolate, registry);
}

void xr_native_type_registry_destroy(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "native_type_registry_destroy: NULL isolate");
    XrNativeTypeRegistry *registry = get_registry(isolate);
    if (!registry) return;
    
    // Free all type infos
    for (uint16_t i = 0; i < registry->count; i++) {
        if (registry->types[i]) {
            xr_free(registry->types[i]);
        }
    }
    
    xr_free(registry->types);
    xr_free(registry);
    xr_isolate_set_native_type_registry(isolate, NULL);
}

// Grow type registry
static void registry_grow(XrNativeTypeRegistry *registry) {
    uint16_t new_capacity = registry->capacity * 2;
    XrNativeTypeInfo **new_types = (XrNativeTypeInfo**)xr_calloc(new_capacity, sizeof(XrNativeTypeInfo*));
    
    memcpy(new_types, registry->types, registry->capacity * sizeof(XrNativeTypeInfo*));
    
    xr_free(registry->types);
    registry->types = new_types;
    registry->capacity = new_capacity;
}

XrNativeTypeInfo* xr_native_registry_register(
    XrayIsolate *isolate,
    const char *name,
    size_t basic_size,
    void (*gc_traverse)(XrGC*, void*),
    void (*finalize)(void*)
) {
    XR_DCHECK(isolate != NULL, "native_registry_register: NULL isolate");
    XR_DCHECK(name != NULL, "native_registry_register: NULL name");
    XrNativeTypeRegistry *registry = get_registry(isolate);
    XR_CHECK(registry != NULL, "Type registry not initialized");
    
    // Check if already registered (by name)
    for (uint16_t i = 0; i < registry->count; i++) {
        if (registry->types[i] && strcmp(registry->types[i]->name, name) == 0) {
            return registry->types[i];  // Return existing type
        }
    }
    
    // Check if need to grow
    if (registry->count >= registry->capacity) {
        registry_grow(registry);
    }
    
    // Check if type IDs exhausted
    if (registry->next_type_id >= XR_TYPE_NATIVE_MAX) {
        return NULL;
    }
    
    // Create new type info
    XrNativeTypeInfo *info = (XrNativeTypeInfo*)xr_malloc(sizeof(XrNativeTypeInfo));
    info->type_id = registry->next_type_id++;
    info->name = name;  // Assumes name is static string
    info->basic_size = basic_size;
    info->gc_traverse = gc_traverse;
    info->finalize = finalize;
    info->method_table = NULL;
    
    // Add to registry
    registry->types[registry->count++] = info;
    
    return info;
}

XrNativeTypeInfo* xr_get_type_info(XrayIsolate *isolate, uint16_t type_id) {
    XrNativeTypeRegistry *registry = get_registry(isolate);
    if (!registry) return NULL;
    
    // Linear search (could optimize with hash table)
    for (uint16_t i = 0; i < registry->count; i++) {
        if (registry->types[i] && registry->types[i]->type_id == type_id) {
            return registry->types[i];
        }
    }
    
    return NULL;
}

XrNativeTypeInfo* xr_get_type_info_by_name(XrayIsolate *isolate, const char *name) {
    XrNativeTypeRegistry *registry = get_registry(isolate);
    if (!registry) return NULL;
    
    for (uint16_t i = 0; i < registry->count; i++) {
        if (registry->types[i] && strcmp(registry->types[i]->name, name) == 0) {
            return registry->types[i];
        }
    }
    
    return NULL;
}

void xr_gc_traverse_native_object(XrGC *gc, XrNativeObject *obj) {
    if (!obj || !obj->type_info) return;
    
    // Call type-specific traverse function
    if (obj->type_info->gc_traverse) {
        obj->type_info->gc_traverse(gc, obj);
    }
}

void xr_finalize_native_object(XrNativeObject *obj) {
    if (!obj || !obj->type_info) return;
    
    // Call type-specific finalizer
    if (obj->type_info->finalize) {
        obj->type_info->finalize(obj);
    }
}
