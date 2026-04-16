/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_type_registry.h - Dynamic native type registration system
 *
 * KEY CONCEPT:
 *   Runtime type registration for third-party extensions without
 *   hardcoding types in the core. Uses type info pointers instead of enum IDs.
 *
 * WHY THIS DESIGN:
 *   - Core types (String/Array/Map) use fixed IDs (0-99)
 *   - Third-party types get dynamically allocated IDs (100+)
 *   - Each type has full metadata (name, GC callbacks, etc.)
 */

#ifndef XNATIVE_TYPE_REGISTRY_H
#define XNATIVE_TYPE_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../base/xdefs.h"

// Forward declarations
struct XrayIsolate;
struct XrGC;

// Type ID ranges
#define XR_TYPE_CORE_START      0
#define XR_TYPE_CORE_END        99
#define XR_TYPE_NATIVE_START    100
#define XR_TYPE_NATIVE_MAX      65535

// Native type info structure
typedef struct XrNativeTypeInfo {
    uint16_t type_id;
    const char *name;
    size_t basic_size;
    
    // GC callbacks
    void (*gc_traverse)(struct XrGC *gc, void *obj);
    void (*finalize)(void *obj);
    
    void *method_table;  // Reserved for method dispatch
} XrNativeTypeInfo;

// Native object header (base for all dynamic type objects)
typedef struct XrNativeObject {
    // GC header (must be first field)
    struct {
        struct XrNativeObject *next;
        unsigned char marked;
        unsigned char padding[7];
    } gc_header;
    
    XrNativeTypeInfo *type_info;
    // User data starts here
} XrNativeObject;

/* ========== Type Registration API ========== */

// Register a native type to registry. Returns existing type info if already registered.
XR_FUNC XrNativeTypeInfo* xr_native_registry_register(
    struct XrayIsolate *isolate,
    const char *name,
    size_t basic_size,
    void (*gc_traverse)(struct XrGC*, void*),
    void (*finalize)(void*)
);

// Get type info by ID
XR_FUNC XrNativeTypeInfo* xr_get_type_info(struct XrayIsolate *isolate, uint16_t type_id);

// Get type info by name
XR_FUNC XrNativeTypeInfo* xr_get_type_info_by_name(struct XrayIsolate *isolate, const char *name);

// Check object type
static inline bool xr_check_native_type(XrNativeObject *obj, XrNativeTypeInfo *expected_type) {
    return obj && obj->type_info == expected_type;
}

// Get object's type name
static inline const char* xr_get_native_type_name(XrNativeObject *obj) {
    return (obj && obj->type_info) ? obj->type_info->name : NULL;
}

/* ========== Internal API (GC only) ========== */

// Initialize type registry
XR_FUNC void xr_native_type_registry_init(struct XrayIsolate *isolate);

// Destroy type registry
XR_FUNC void xr_native_type_registry_destroy(struct XrayIsolate *isolate);

// Traverse native object (called by GC)
XR_FUNC void xr_gc_traverse_native_object(struct XrGC *gc, XrNativeObject *obj);

// Finalize native object (called by GC)
XR_FUNC void xr_finalize_native_object(XrNativeObject *obj);

#endif // XNATIVE_TYPE_REGISTRY_H
