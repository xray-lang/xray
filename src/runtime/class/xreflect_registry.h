/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_registry.h - Type registry for reflection system
 *
 * KEY CONCEPT:
 *   Global registry storing metadata for all types.
 *   Supports O(1) lookup by name via hashmap.
 *   Auto-registers types when classes are created.
 */

#ifndef XREFLECT_REGISTRY_H
#define XREFLECT_REGISTRY_H

#include "xreflect_internal.h"
#include "../../base/xhashmap.h"
#include <stdbool.h>

// Forward declarations via xforward_decl.h

/* ========== Type Registry ========== */

struct XrTypeRegistry {
    XrTypeMetadata **types;
    int type_count;
    int capacity;

    XrHashMap *type_map;  // name -> XrTypeMetadata*

    // Builtin type cache
    XrTypeMetadata *int_type;
    XrTypeMetadata *float_type;
    XrTypeMetadata *bool_type;
    XrTypeMetadata *string_type;
    XrTypeMetadata *array_type;
    XrTypeMetadata *map_type;
    XrTypeMetadata *object_type;
    XrTypeMetadata *null_type;

    bool is_initialized;
};

/* ========== Lifecycle ========== */

XR_FUNC void xr_registry_init(XrayIsolate *X);
XR_FUNC void xr_registry_free(XrayIsolate *X);

/* ========== Registration ========== */

// Returns false if type already exists
XR_FUNC bool xr_registry_register_type(XrayIsolate *X, XrTypeMetadata *meta);

// Auto-creates metadata from XrClass
XR_FUNC XrTypeMetadata *xr_registry_register_class(XrayIsolate *X, XrClass *klass);

XR_FUNC bool xr_registry_unregister_type(XrayIsolate *X, const char *name);

/* ========== Lookup ========== */

XR_FUNC XrTypeMetadata *xr_registry_find_type(XrayIsolate *X, const char *name);
XR_FUNC XrTypeMetadata *xr_registry_find_type_by_class(XrayIsolate *X, XrClass *klass);

// Caller must free the returned array
XR_FUNC XrTypeMetadata **xr_registry_get_all_types(XrayIsolate *X, int *count);

/* ========== Builtin Type Accessors ========== */

XR_FUNC XrTypeMetadata *xr_registry_get_int_type(XrayIsolate *X);
XR_FUNC XrTypeMetadata *xr_registry_get_float_type(XrayIsolate *X);
XR_FUNC XrTypeMetadata *xr_registry_get_bool_type(XrayIsolate *X);
XR_FUNC XrTypeMetadata *xr_registry_get_string_type(XrayIsolate *X);
XR_FUNC XrTypeMetadata *xr_registry_get_array_type(XrayIsolate *X);
XR_FUNC XrTypeMetadata *xr_registry_get_map_type(XrayIsolate *X);
XR_FUNC XrTypeMetadata *xr_registry_get_object_type(XrayIsolate *X);
XR_FUNC XrTypeMetadata *xr_registry_get_null_type(XrayIsolate *X);

/* ========== Metadata ========== */

// Collect fields/methods metadata, handle inheritance
XR_FUNC void xr_registry_initialize_metadata(XrayIsolate *X, XrTypeMetadata *meta);

/* ========== Debug ========== */

XR_FUNC void xr_registry_print(XrayIsolate *X);
XR_FUNC void xr_registry_get_stats(XrayIsolate *X, int *total, int *classes, int *interfaces,
                                   int *builtins);

/* ========== Static Analyzer Type Integration ========== */

// Forward declaration
struct XrType;

// Convert static XrType to runtime TypeMetadata
// Creates metadata if not already registered
XR_FUNC XrTypeMetadata *xr_registry_from_xa_type(XrayIsolate *X, struct XrType *xa_type);

// Get XrType flags as runtime type kind string (for debugging)
XR_FUNC const char *xr_xr_type_kind_name(struct XrType *xa_type);

#endif  // XREFLECT_REGISTRY_H
