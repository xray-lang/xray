/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_registry.c - Type registry implementation
 */

#include "xreflect_registry.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xreflect_internal.h"
#include "xclass_system.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../value/xtype_names.h"
#include "../symbol/xsymbol_table.h"
#include "../../base/xhashmap.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../xglobals_table.h"

// #define REFLECTION_DEBUG
#ifdef REFLECTION_DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) do {} while(0)
#endif

#define REGISTRY_INITIAL_CAPACITY 64

/* ========== Zero-copy Helper ========== */

// Create metadata with pointer to XrClass (no copy)
static inline XrTypeMetadata* xr_metadata_create_type_zerocopy(XrayIsolate *X, XrClass *klass) {
    (void)X;
    XrTypeMetadata *meta = (XrTypeMetadata*)xr_malloc(sizeof(XrTypeMetadata));
    if (!meta) return NULL;
    
    meta->klass = klass;
    meta->name = NULL;
    return meta;
}

/* ========== Lifecycle ========== */

void xr_registry_init(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "registry_init: NULL isolate");
    XrTypeRegistry *registry = (XrTypeRegistry*)xr_malloc(sizeof(XrTypeRegistry));
    if (!registry) {
        xr_log_warning("reflect", "Failed to allocate XrTypeRegistry");
        return;
    }
    
    registry->capacity = REGISTRY_INITIAL_CAPACITY;
    registry->types = (XrTypeMetadata**)xr_malloc(registry->capacity * sizeof(XrTypeMetadata*));
    if (registry->types) {
        memset(registry->types, 0, registry->capacity * sizeof(XrTypeMetadata*));
    }
    registry->type_count = 0;
    registry->type_map = xr_hashmap_new();
    
    registry->int_type = NULL;
    registry->float_type = NULL;
    registry->bool_type = NULL;
    registry->string_type = NULL;
    registry->array_type = NULL;
    registry->map_type = NULL;
    registry->object_type = NULL;
    registry->null_type = NULL;
    
    registry->is_initialized = true;
    xr_isolate_set_type_registry(X, registry);
}

void xr_registry_free(XrayIsolate *X) {
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry) return;
    
    // Clear hashmap first to avoid dangling pointers
    if (registry->type_map) {
        xr_hashmap_clear(registry->type_map);
    }
    
    for (int i = 0; i < registry->type_count; i++) {
        if (registry->types[i]) {
            xr_free(registry->types[i]);
        }
    }
    
    if (registry->type_map) {
        xr_hashmap_free(registry->type_map);
    }
    
    xr_free(registry->types);
    xr_free(registry);
    xr_isolate_set_type_registry(X, NULL);
}

/* ========== Internal Helpers ========== */

static void registry_grow(XrTypeRegistry *registry) {
    int new_capacity = registry->capacity * 2;
    XrTypeMetadata **new_types = (XrTypeMetadata**)xr_realloc(registry->types, sizeof(XrTypeMetadata*) * new_capacity);
    if (!new_types) {
        xr_log_warning("reflect", "failed to grow registry");
        return;
    }
    
    registry->types = new_types;
    for (int i = registry->capacity; i < new_capacity; i++) {
        registry->types[i] = NULL;
    }
    registry->capacity = new_capacity;
}

static int registry_find_index(XrTypeRegistry *registry, const char *name) {
    if (registry->type_map) {
        XrTypeMetadata *meta = (XrTypeMetadata*)xr_hashmap_get(registry->type_map, name);
        if (meta) {
            for (int i = 0; i < registry->type_count; i++) {
                if (registry->types[i] == meta) {
                    return i;
                }
            }
        }
        return -1;
    }
    
    // Fallback: linear search
    for (int i = 0; i < registry->type_count; i++) {
        if (registry->types[i] && strcmp(registry->types[i]->klass->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* ========== Registration ========== */

bool xr_registry_register_type(XrayIsolate *X, XrTypeMetadata *meta) {
    XR_DCHECK(X != NULL, "registry_register_type: NULL isolate");
    XR_DCHECK(meta != NULL, "registry_register_type: NULL meta");
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry || !meta) return false;
    
    const char *type_name = meta->klass ? meta->klass->name : meta->name;
    if (!type_name) return false;
    
    if (registry_find_index(registry, type_name) >= 0) {
        xr_log_warning("reflect", "type '%s' already registered", type_name);
        return false;
    }
    
    if (registry->type_count >= registry->capacity) {
        registry_grow(registry);
    }
    
    int index = registry->type_count++;
    XR_DCHECK(registry->type_count <= registry->capacity, "registry_register: count > capacity");
    registry->types[index] = meta;
    
    if (registry->type_map) {
        xr_hashmap_set(registry->type_map, type_name, meta);
    }
    
    // Cache on class for O(1) reverse lookup
    if (meta->klass) {
        meta->klass->type_metadata = meta;
    }
    
    // Cache builtin types
    if (meta->klass) {
        if (strcmp(type_name, TYPE_NAME_INT) == 0) {
            registry->int_type = meta;
        } else if (strcmp(type_name, TYPE_NAME_FLOAT) == 0) {
            registry->float_type = meta;
        } else if (strcmp(type_name, TYPE_NAME_BOOL) == 0) {
            registry->bool_type = meta;
        } else if (strcmp(type_name, TYPE_NAME_STRING) == 0) {
            registry->string_type = meta;
        } else if (strcmp(type_name, TYPE_NAME_ARRAY) == 0) {
            registry->array_type = meta;
        } else if (strcmp(type_name, TYPE_NAME_MAP) == 0) {
            registry->map_type = meta;
        } else if (strcmp(type_name, TYPE_NAME_OBJECT) == 0) {
            registry->object_type = meta;
        } else if (strcmp(type_name, TYPE_NAME_NULL) == 0) {
            registry->null_type = meta;
        }
    }
    
    return true;
}

// Lazy initialization: metadata created at registration, initialized on first access
XrTypeMetadata* xr_registry_register_class(XrayIsolate *X, XrClass *klass) {
    XR_DCHECK(X != NULL, "registry_register_class: NULL isolate");
    if (!klass) return NULL;
    
    XrTypeMetadata *existing = xr_registry_find_type(X, klass->name);
    if (existing) return existing;
    
    XrTypeMetadata *meta = xr_metadata_create_type_zerocopy(X, klass);
    if (!meta) return NULL;
    
    if (!xr_registry_register_type(X, meta)) {
        xr_free(meta);
        return NULL;
    }
    
    return meta;
}

bool xr_registry_unregister_type(XrayIsolate *X, const char *name) {
    XR_DCHECK(X != NULL, "registry_unregister_type: NULL isolate");
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry || !name) return false;
    
    int index = registry_find_index(registry, name);
    if (index < 0) return false;
    
    if (registry->type_map) {
        xr_hashmap_delete(registry->type_map, name);
    }
    
    xr_free(registry->types[index]);
    
    // Swap with last element
    int last = registry->type_count - 1;
    if (index != last) {
        registry->types[index] = registry->types[last];
    }
    registry->types[last] = NULL;
    registry->type_count--;
    
    return true;
}

/* ========== Lookup ========== */

static XrTypeMetadata* resolve_type_by_name(XrayIsolate *X, const char *type_name);

// Supports lazy registration
XrTypeMetadata* xr_registry_find_type(XrayIsolate *X, const char *name) {
    XR_DCHECK(X != NULL, "registry_find_type: NULL isolate");
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry || !name) return NULL;
    
    if (registry->type_map) {
        XrTypeMetadata *cached = (XrTypeMetadata*)xr_hashmap_get(registry->type_map, name);
        if (cached) return cached;
    }
    
    return resolve_type_by_name(X, name);
}

XrTypeMetadata* xr_registry_find_type_by_class(XrayIsolate *X, XrClass *klass) {
    XR_DCHECK(X != NULL, "registry_find_type_by_class: NULL isolate");
    if (!klass) return NULL;
    
    // O(1) cached lookup
    if (klass->type_metadata) return klass->type_metadata;
    
    // Fallback: linear scan (first access before registration)
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry) return NULL;
    
    for (int i = 0; i < registry->type_count; i++) {
        if (registry->types[i]->klass == klass) {
            klass->type_metadata = registry->types[i];
            return registry->types[i];
        }
    }
    return NULL;
}

static XrClass* find_builtin_class_by_name(XrayIsolate *X, const char *name) {
    if (!X || !xr_isolate_get_core_classes(X) || !name) return NULL;
    
    XrClass *result = NULL;
    if (strcasecmp(name, TYPE_NAME_STRING) == 0) result = xr_isolate_get_core_classes(X)->stringClass;
    else if (strcasecmp(name, TYPE_NAME_INT) == 0) result = xr_isolate_get_core_classes(X)->intClass;
    else if (strcasecmp(name, TYPE_NAME_FLOAT) == 0) result = xr_isolate_get_core_classes(X)->floatClass;
    else if (strcasecmp(name, TYPE_NAME_BOOL) == 0) result = xr_isolate_get_core_classes(X)->boolClass;
    else if (strcasecmp(name, TYPE_NAME_ARRAY) == 0) result = xr_isolate_get_core_classes(X)->arrayClass;
    else if (strcasecmp(name, TYPE_NAME_MAP) == 0) result = xr_isolate_get_core_classes(X)->mapClass;
    else if (strcasecmp(name, TYPE_NAME_SET) == 0) result = xr_isolate_get_core_classes(X)->setClass;
    else if (strcasecmp(name, TYPE_NAME_OBJECT) == 0) result = xr_isolate_get_core_classes(X)->objectClass;
    
    if (result && result->name) return result;
    return NULL;
}

// Called by xr_registry_find_type when cache miss, do NOT call xr_registry_find_type here
static XrTypeMetadata* resolve_type_by_name(XrayIsolate *X, const char *type_name) {
    if (!X || !type_name) return NULL;
    
    XrTypeMetadata *meta = NULL;
    
    // Special case: void has no XrClass
    if (strcasecmp(type_name, "void") == 0) {
        meta = (XrTypeMetadata*)xr_malloc(sizeof(XrTypeMetadata));
        if (meta) {
            meta->klass = NULL;
            meta->name = "void";
            if (xr_registry_register_type(X, meta)) {
                return meta;
            }
            xr_free(meta);
        }
        return NULL;
    }
    
    // Try builtin class
    XrClass *builtin_class = find_builtin_class_by_name(X, type_name);
    if (builtin_class) {
        meta = xr_registry_find_type_by_class(X, builtin_class);
        if (meta) return meta;
        
        meta = xr_metadata_create_type_zerocopy(X, builtin_class);
        if (meta && xr_registry_register_type(X, meta)) {
            return meta;
        }
        if (meta) xr_free(meta);
    }
    
    // Try user-defined class (lazy registration)
    XrGlobalsTable *globals = xr_isolate_get_globals(X);
    if (globals) {
        size_t count = xr_globals_count(globals);
        for (size_t i = 0; i < count; i++) {
            XrValue val = xr_globals_get(globals, (int)i);
            if (XR_IS_CLASS(val)) {
                XrClass *klass = XR_TO_CLASS(val);
                if (klass && klass->name && strcmp(klass->name, type_name) == 0) {
                    meta = xr_metadata_create_type_zerocopy(X, klass);
                    if (meta && xr_registry_register_type(X, meta)) {
                        return meta;
                    }
                    if (meta) xr_free(meta);
                }
            }
        }
    }
    
    return NULL;
}

XrTypeMetadata** xr_registry_get_all_types(XrayIsolate *X, int *count) {
    XR_DCHECK(X != NULL, "registry_get_all_types: NULL isolate");
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry || !count) return NULL;
    
    *count = registry->type_count;
    if (registry->type_count == 0) return NULL;
    
    XrTypeMetadata **result = (XrTypeMetadata**)xr_malloc(sizeof(XrTypeMetadata*) * registry->type_count);
    for (int i = 0; i < registry->type_count; i++) {
        result[i] = registry->types[i];
    }
    return result;
}

/* ========== Builtin Type Accessors ========== */

XrTypeMetadata* xr_registry_get_int_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->int_type : NULL;
}

XrTypeMetadata* xr_registry_get_float_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->float_type : NULL;
}

XrTypeMetadata* xr_registry_get_bool_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->bool_type : NULL;
}

XrTypeMetadata* xr_registry_get_string_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->string_type : NULL;
}

XrTypeMetadata* xr_registry_get_array_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->array_type : NULL;
}

XrTypeMetadata* xr_registry_get_map_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->map_type : NULL;
}

XrTypeMetadata* xr_registry_get_object_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->object_type : NULL;
}

XrTypeMetadata* xr_registry_get_null_type(XrayIsolate *X) {
    XrTypeRegistry *r = xr_isolate_get_type_registry(X);
    return r ? r->null_type : NULL;
}

/* ========== Metadata ========== */

// Zero-copy: all data accessed directly from XrClass
void xr_registry_initialize_metadata(XrayIsolate *X, XrTypeMetadata *meta) {
    (void)X;
    (void)meta;
}

/* ========== Debug ========== */

void xr_registry_print(XrayIsolate *X) {
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry) {
        printf("XrTypeRegistry: NULL\n");
        return;
    }
    
    printf("=== Type Registry ===\n");
    printf("Registered types: %d / %d\n", registry->type_count, registry->capacity);
    printf("\nTypes:\n");
    
    for (int i = 0; i < registry->type_count; i++) {
        XrTypeMetadata *meta = registry->types[i];
        printf("  [%2d] %s - %d fields, %d methods\n", 
               i, meta->klass->name, meta->klass->field_count, meta->klass->method_count);
    }
}

void xr_registry_get_stats(XrayIsolate *X, int *total, int *classes, 
                           int *interfaces, int *builtins) {
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry) {
        if (total) *total = 0;
        if (classes) *classes = 0;
        if (interfaces) *interfaces = 0;
        if (builtins) *builtins = 0;
        return;
    }
    
    if (total) *total = registry->type_count;
    if (classes) *classes = registry->type_count;
    if (interfaces) *interfaces = 0;
    if (builtins) *builtins = 0;
}

/* ========== Static Analyzer Type Integration ========== */

#include "../value/xtype.h"

const char* xr_xr_type_kind_name(struct XrType *xa_type) {
    if (!xa_type) return "object";
    
    switch (xa_type->kind) {
    case XR_KIND_INT:       return "int";
    case XR_KIND_FLOAT:     return "float";
    case XR_KIND_BOOL:      return "bool";
    case XR_KIND_STRING:    return "string";
    case XR_KIND_NULL:      return "null";
    case XR_KIND_ARRAY:     return "Array";
    case XR_KIND_MAP:       return "Map";
    case XR_KIND_CLASS:     return "class";
    case XR_KIND_INSTANCE:  return "instance";
    case XR_KIND_INTERFACE: return "interface";
    case XR_KIND_FUNCTION:  return "function";
    default: break;
    }
    
    return "object";
}

XrTypeMetadata* xr_registry_from_xa_type(XrayIsolate *X, struct XrType *xa_type) {
    if (!X || !xa_type) return NULL;
    
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (!registry) return NULL;
    
    // For primitive types, return cached metadata
    switch (xa_type->kind) {
    case XR_KIND_INT:    return registry->int_type;
    case XR_KIND_FLOAT:  return registry->float_type;
    case XR_KIND_BOOL:   return registry->bool_type;
    case XR_KIND_STRING: return registry->string_type;
    case XR_KIND_NULL:   return registry->null_type;
    case XR_KIND_ARRAY:  return registry->array_type;
    case XR_KIND_MAP:    return registry->map_type;
    default: break;
    }
    
    // For class/instance types, look up by name
    if ((xa_type->kind == XR_KIND_CLASS || xa_type->kind == XR_KIND_INSTANCE) && 
        xa_type->instance.class_name) {
        return xr_registry_find_type(X, xa_type->instance.class_name);
    }
    
    // Default to object type
    return registry->object_type;
}

