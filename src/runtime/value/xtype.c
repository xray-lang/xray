/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype.c - Static type system implementation
 */

#include "xtype.h"
#include "../class/xclass_info.h"
#include "xtype_pool.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../../shared/xr_derive_flags.h"
#include "../../os/os_thread.h"
#include "xtype_names.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "xtype_internal.h"

XR_FUNC bool xr_conversion_scalar_rep_is_integer(uint8_t scalar_rep) {
    return xr_scalar_rep_is_integer(scalar_rep);
}

bool xr_type_is_json_value(const XrType *type) {
    if (!type)
        return false;
    switch (type->kind) {
        case XR_KIND_NULL:
        case XR_KIND_BOOL:
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_STRING:
        case XR_KIND_JSON:
            return true;
        case XR_KIND_ARRAY:
            /* Array<Json> is the domain's own array form. A typed container
             * stays outside it however encodable its elements are: Array<int>
             * is an int container that can be encoded, not a Json value. */
            return type->container.element_type &&
                   type->container.element_type->kind == XR_KIND_JSON;
        case XR_KIND_MAP:
            return xr_type_is_json_object(type);
        case XR_KIND_UNION:
            if (type->union_type.member_count <= 0 || !type->union_type.members)
                return false;
            for (int i = 0; i < type->union_type.member_count; i++) {
                if (!xr_type_is_json_value(type->union_type.members[i]))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

uint8_t xr_type_json_value_kind(const XrType *type) {
    uint8_t kind;
    if (!type)
        return XR_JSON_VALUE_ANY;
    switch (type->kind) {
        case XR_KIND_NULL:
            kind = XR_JSON_VALUE_NULL;
            break;
        case XR_KIND_BOOL:
            kind = XR_JSON_VALUE_BOOL;
            break;
        case XR_KIND_INT:
            kind = XR_JSON_VALUE_INT;
            break;
        case XR_KIND_FLOAT:
            kind = XR_JSON_VALUE_FLOAT;
            break;
        case XR_KIND_STRING:
            kind = XR_JSON_VALUE_STRING;
            break;
        case XR_KIND_JSON:
            kind = XR_JSON_VALUE_JSON;
            break;
        case XR_KIND_STRUCT_OBJECT:
            kind = XR_JSON_VALUE_STRUCT_OBJECT;
            break;
        case XR_KIND_ARRAY:
            if (type->container.element_type) {
                kind = XR_JSON_VALUE_ARRAY;
                break;
            }
            return XR_JSON_VALUE_ANY;
        case XR_KIND_MAP:
            if (type->map.key_type && XR_TYPE_IS_STRING(type->map.key_type) &&
                !type->map.key_type->is_nullable && type->map.value_type) {
                kind = XR_JSON_VALUE_MAP;
                break;
            }
            return XR_JSON_VALUE_ANY;
        case XR_KIND_ENUM:
            if (type->enum_type.layout && type->enum_type.layout->is_zero_payload &&
                type->enum_type.layout->variant_count > 0) {
                kind = XR_JSON_VALUE_ENUM;
                break;
            }
            return XR_JSON_VALUE_ANY;
        case XR_KIND_INSTANCE:
            if (type->instance.class_ref && type->instance.class_name &&
                (type->instance.class_ref->derive_flags & XR_DERIVE_JSON) != 0) {
                kind = XR_JSON_VALUE_CLASS_INSTANCE;
                break;
            }
            return XR_JSON_VALUE_ANY;
        default:
            return XR_JSON_VALUE_ANY;
    }
    return kind | (type->is_nullable ? XR_JSON_VALUE_NULLABLE : 0u);
}

static bool xr_type_is_json_decode_field_supported_depth(const XrType *type, int depth) {
    if (!type || depth > 16)
        return false;
    /* Derive authorization is analyzer-owned; the value layer only records
     * that a declared class has enough identity for a compiler sidecar. */
    if (XR_TYPE_IS_INSTANCE(type))
        return type->instance.class_ref != NULL && type->instance.class_name != NULL;
    uint8_t base = xr_json_value_kind_base(xr_type_json_value_kind(type));
    if (base == XR_JSON_VALUE_STRUCT_OBJECT) {
        if (!XR_TYPE_IS_STRUCT_OBJECT(type) || !xr_type_is_exact_struct_object(type) ||
            type->object.field_count <= 0 || !type->object.field_names || !type->object.field_types)
            return false;
        for (int i = 0; i < type->object.field_count; i++) {
            if (!xr_type_is_json_decode_field_supported_depth(type->object.field_types[i],
                                                              depth + 1))
                return false;
        }
        return true;
    }
    if (base == XR_JSON_VALUE_ARRAY) {
        return XR_TYPE_IS_ARRAY(type) && type->container.element_type &&
               xr_type_is_json_decode_field_supported_depth(type->container.element_type,
                                                            depth + 1);
    }
    if (base == XR_JSON_VALUE_MAP) {
        return XR_TYPE_IS_MAP(type) && type->map.key_type &&
               XR_TYPE_IS_STRING(type->map.key_type) && !type->map.key_type->is_nullable &&
               type->map.value_type &&
               xr_type_is_json_decode_field_supported_depth(type->map.value_type, depth + 1);
    }
    if (base == XR_JSON_VALUE_ENUM) {
        return XR_TYPE_IS_ENUM(type) && type->enum_type.layout &&
               type->enum_type.layout->is_zero_payload && type->enum_type.layout->variant_count > 0;
    }
    return base != XR_JSON_VALUE_ANY;
}

bool xr_type_is_json_decode_field_supported(const XrType *type) {
    return xr_type_is_json_decode_field_supported_depth(type, 0);
}

// ========== Process-level static singletons (early init) ==========
// Basic types are immutable and globally shared. No allocation needed.
static xr_once_t g_types_once = XR_ONCE_INITIALIZER;

// Non-nullable singletons
static XrType g_type_int;
static XrType g_type_float;
static XrType g_type_string;
static XrType g_type_bool;
static XrType g_type_rune;
static XrType g_type_null;
static XrType g_type_unknown;
static XrType g_type_error;
static XrType g_type_never;
static XrType g_type_unit;
static XrType g_type_json;

// Nullable singletons (T?)
static XrType g_type_int_nullable;
static XrType g_type_float_nullable;
static XrType g_type_string_nullable;
static XrType g_type_bool_nullable;
static XrType g_type_rune_nullable;

static XR_THREAD_LOCAL XrTypePool *g_current_type_pool = NULL;

static void init_singleton(XrType *t, XrTypeKind kind, uint32_t id, bool nullable,
                           uint8_t scalar_rep) {
    memset(t, 0, sizeof(XrType));
    t->kind = kind;
    t->id = id;
    t->frozen = true;
    t->is_nullable = nullable;
    t->scalar_rep = scalar_rep;
}

static void xr_type_global_init_once(void) {
    uint32_t id = 1;
    init_singleton(&g_type_int, XR_KIND_INT, id++, false, XR_NATIVE_I64);
    init_singleton(&g_type_float, XR_KIND_FLOAT, id++, false, XR_NATIVE_F64);
    init_singleton(&g_type_string, XR_KIND_STRING, id++, false, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_bool, XR_KIND_BOOL, id++, false, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_rune, XR_KIND_RUNE, id++, false, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_null, XR_KIND_NULL, id++, false, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_unknown, XR_KIND_UNKNOWN, id++, false, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_error, XR_KIND_ERROR, id++, false, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_never, XR_KIND_NEVER, id++, false, XR_SCALAR_REP_NONE);
    // Unit type singleton: dedicated XR_KIND_UNIT kind, spelled `()` in user
    // syntax. Acts as the canonical "no meaningful value" type for functions
    // returning nothing and as the unique value of the empty tuple literal.
    init_singleton(&g_type_unit, XR_KIND_UNIT, id++, false, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_json, XR_KIND_JSON, id++, false, XR_SCALAR_REP_NONE);

    init_singleton(&g_type_int_nullable, XR_KIND_INT, id++, true, XR_NATIVE_I64);
    init_singleton(&g_type_float_nullable, XR_KIND_FLOAT, id++, true, XR_NATIVE_F64);
    init_singleton(&g_type_string_nullable, XR_KIND_STRING, id++, true, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_bool_nullable, XR_KIND_BOOL, id++, true, XR_SCALAR_REP_NONE);
    init_singleton(&g_type_rune_nullable, XR_KIND_RUNE, id++, true, XR_SCALAR_REP_NONE);
}

void xr_type_global_init(void) {
    xr_once_call(&g_types_once, xr_type_global_init_once);
}

// Release process-level type-system state (task 218 defense line 4).
// The basic-type singletons are static storage (nothing to free), but the
// per-thread "current type pool" is a *borrowed* pointer into analyzer/isolate
// memory. Clearing it on shutdown guarantees no stale cross-lifetime borrow
// (R-OWN-1) outlives the pool it points at. Idempotent.
void xr_type_global_shutdown(void) {
    g_current_type_pool = NULL;
}

// Set the analyzer/current type pool for no-isolate type helpers.
void xr_type_set_current_pool(XrTypePool *pool, uint32_t *id_counter) {
    (void) id_counter;  // ID counter now managed by pool itself
    g_current_type_pool = pool;
}

// Get current type pool for rare no-X contexts like xr_type_to_string.
XrTypePool *xr_type_get_current_pool(void) {
    return g_current_type_pool;
}

static inline XrVMRuntime *resolve_isolate(XrVMRuntime *X) {
    return X;
}

static inline XrTypePool *resolve_type_pool(XrVMRuntime *X) {
    (void) X;
    return g_current_type_pool;
}

// Helper: allocate and initialize a type (for non-singleton types)
// Uses pool arena for allocation - memory freed when pool is reset/destroyed
static XrType *type_alloc(XrVMRuntime *X, XrTypeKind kind) {
    X = resolve_isolate(X);
    XrTypePool *pool = resolve_type_pool(X);
    XR_CHECK(pool != NULL, "Type pool not set - call xr_type_set_current_pool first");
    XrType *type = xr_pool_alloc_type(pool, kind);
    if (type) {
        type->scalar_rep = kind == XR_KIND_INT     ? XR_NATIVE_I64
                           : kind == XR_KIND_FLOAT ? XR_NATIVE_F64
                                                   : XR_SCALAR_REP_NONE;
    }
    return type;
}

static void *type_alloc_array(XrTypePool *pool, size_t elem_size, int count, size_t *out_size) {
    if (!pool || elem_size == 0 || count <= 0)
        return NULL;
    size_t n = (size_t) count;
    if (n > SIZE_MAX / elem_size)
        return NULL;
    size_t size = elem_size * n;
    void *ptr = xr_pool_alloc_array(pool, elem_size, n);
    if (ptr && out_size)
        *out_size = size;
    return ptr;
}

XrType *xr_type_new(XrVMRuntime *X, XrTypeKind kind) {
    return type_alloc(X, kind);
}

// Primitive type constructors (return process-level singletons)
XrType *xr_type_new_int(XrVMRuntime *X) {
    (void) X;
    return &g_type_int;
}
XrType *xr_type_new_float(XrVMRuntime *X) {
    (void) X;
    return &g_type_float;
}
XrType *xr_type_new_string(XrVMRuntime *X) {
    (void) X;
    return &g_type_string;
}
XrType *xr_type_new_bool(XrVMRuntime *X) {
    (void) X;
    return &g_type_bool;
}
XrType *xr_type_new_rune(XrVMRuntime *X) {
    (void) X;
    return &g_type_rune;
}
XrType *xr_type_new_null(XrVMRuntime *X) {
    (void) X;
    return &g_type_null;
}
XrType *xr_type_new_unknown(XrVMRuntime *X) {
    (void) X;
    return &g_type_unknown;
}
XrType *xr_type_new_error(XrVMRuntime *X) {
    (void) X;
    return &g_type_error;
}
XrType *xr_type_new_never(XrVMRuntime *X) {
    (void) X;
    return &g_type_never;
}
// Unit type singleton (XR_KIND_UNIT, spelled `()` in user syntax). Returns
// the same singleton regardless of isolate to enable pointer equality.
XrType *xr_type_new_unit(XrVMRuntime *X) {
    (void) X;
    return &g_type_unit;
}

XrType *xr_type_new_int_width(XrVMRuntime *X, int width) {
    if (width == XR_NATIVE_I64)
        return xr_type_new_int(X);
    XrType *type = type_alloc(X, XR_KIND_INT);
    if (!type)
        return NULL;
    type->scalar_rep = (uint8_t) width;
    return type;
}

XrType *xr_type_new_float_width(XrVMRuntime *X, int width) {
    if (width == XR_NATIVE_F64)
        return xr_type_new_float(X);
    XrType *type = type_alloc(X, XR_KIND_FLOAT);
    if (!type)
        return NULL;
    type->scalar_rep = (uint8_t) width;
    return type;
}

// Container type constructors
XrType *xr_type_new_array(XrVMRuntime *X, XrType *element_type) {
    XR_DCHECK(element_type != NULL, "type_new_array: NULL element_type");
    XrType *type = type_alloc(X, XR_KIND_ARRAY);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    return type;
}

XrType *xr_type_new_slice(XrVMRuntime *X, XrType *element_type) {
    XR_DCHECK(element_type != NULL, "type_new_slice: NULL element_type");
    XrType *type = type_alloc(X, XR_KIND_SLICE);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    return type;
}

XrType *xr_type_new_map(XrVMRuntime *X, XrType *key_type, XrType *value_type) {
    XR_DCHECK(key_type != NULL, "type_new_map: NULL key_type");
    XR_DCHECK(value_type != NULL, "type_new_map: NULL value_type");
    XrType *type = type_alloc(X, XR_KIND_MAP);
    if (!type)
        return NULL;
    type->map.key_type = key_type;
    type->map.value_type = value_type;
    return type;
}

XrType *xr_type_new_set(XrVMRuntime *X, XrType *element_type) {
    XrType *type = type_alloc(X, XR_KIND_SET);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    return type;
}

XrType *xr_type_new_channel(XrVMRuntime *X, XrType *element_type) {
    XrType *type = type_alloc(X, XR_KIND_CHANNEL);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    return type;
}

XrType *xr_type_new_pointer(XrVMRuntime *X, XrType *element_type, bool is_mut) {
    XrType *type = type_alloc(X, XR_KIND_POINTER);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    type->ptr_is_mut = is_mut;
    return type;
}

XrType *xr_type_new_task(XrVMRuntime *X, XrType *result_type) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (!type)
        return NULL;
    type->instance.class_name = "Task";
    if (result_type) {
        XrTypePool *pool = resolve_type_pool(X);
        XrType **args = (XrType **) xr_pool_alloc(pool, sizeof(XrType *));
        if (args) {
            args[0] = result_type;
            type->instance.type_args = args;
            type->instance.type_arg_count = 1;
        }
    }
    return type;
}

// Object types
XrType *xr_type_new_json(XrVMRuntime *X) {
    (void) X;
    return &g_type_json;  // Process-level singleton (plain Json without fields)
}

static XrType *xr_type_new_object_shape(XrVMRuntime *X, const char **names, XrType **types,
                                        int count) {
    if (count < 0)
        return NULL;
    if (count > 0 && (!names || !types))
        return NULL;
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_STRUCT_OBJECT);
    if (!type)
        return NULL;
    XrTypePool *pool = resolve_type_pool(X);
    if (count > 0 && names && types) {
        const char **field_names =
            (const char **) type_alloc_array(pool, sizeof(char *), count, NULL);
        XrType **field_types = (XrType **) type_alloc_array(pool, sizeof(XrType *), count, NULL);
        if (!field_names || !field_types)
            return NULL;
        type->object.field_count = count;
        type->object.field_names = field_names;
        type->object.field_types = field_types;
        for (int i = 0; i < count; i++) {
            type->object.field_names[i] = names[i] ? xr_pool_strdup(pool, names[i]) : NULL;
            type->object.field_types[i] = types[i];
        }
    }
    return type;
}

XrType *xr_type_new_struct_object_with_fields(XrVMRuntime *X, const char **names, XrType **types,
                                              int count) {
    return xr_type_new_object_shape(X, names, types, count);
}

uint64_t xr_type_stable_key(const XrType *type) {
    if (!type)
        return 0;
    const char *canonical = xr_type_to_string((XrType *) type);
    uint64_t key = xr_hash_bytes64(canonical ? canonical : "<error>",
                                   canonical ? strlen(canonical) : sizeof("<error>") - 1);
    return key ? key : 1;
}

XR_FUNC void xr_type_set_object_field_readonly(XrVMRuntime *X, XrType *type, const bool *readonly,
                                               int count) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || !readonly || count <= 0 ||
        count != type->object.field_count)
        return;
    X = resolve_isolate(X);
    XrTypePool *pool = resolve_type_pool(X);
    if (!pool)
        return;
    type->object.field_readonly = (bool *) type_alloc_array(pool, sizeof(bool), count, NULL);
    if (!type->object.field_readonly)
        return;
    memcpy(type->object.field_readonly, readonly, sizeof(bool) * (size_t) count);
}

XR_FUNC void xr_type_set_object_type_name(XrVMRuntime *X, XrType *type, const char *name) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type) || !name)
        return;
    X = resolve_isolate(X);
    XrTypePool *pool = resolve_type_pool(X);
    if (!pool)
        return;
    type->object.type_name = xr_pool_strdup(pool, name);
}

XR_FUNC void xr_type_set_json_field_readonly(XrVMRuntime *X, XrType *type, const bool *readonly,
                                             int count) {
    xr_type_set_object_field_readonly(X, type, readonly, count);
}

XR_FUNC void xr_type_set_json_type_name(XrVMRuntime *X, XrType *type, const char *name) {
    xr_type_set_object_type_name(X, type, name);
}

// Optional type (T?) - unified: uses is_nullable on the base type itself
XrType *xr_type_new_optional(XrVMRuntime *X, XrType *base_type) {
    return xr_type_make_nullable(X, base_type);
}

XrType *xr_type_get_base(XrType *optional_type) {
    return xr_type_non_nullable(NULL, optional_type);
}

static bool xr_type_contains_error_impl(const XrType *type, int depth) {
    if (!type || depth > 64)
        return false;
    if (XR_TYPE_IS_ERROR(type))
        return true;

    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_SLICE:
        case XR_KIND_POINTER:
            return xr_type_contains_error_impl(type->container.element_type, depth + 1);
        case XR_KIND_FIXED_ARRAY:
            return xr_type_contains_error_impl(type->fixed_array.element_type, depth + 1);
        case XR_KIND_MAP:
            return xr_type_contains_error_impl(type->map.key_type, depth + 1) ||
                   xr_type_contains_error_impl(type->map.value_type, depth + 1);
        case XR_KIND_JSON:
        case XR_KIND_STRUCT_OBJECT:
            for (int i = 0; i < type->object.field_count; i++) {
                XrType *field_type = type->object.field_types ? type->object.field_types[i] : NULL;
                if (xr_type_contains_error_impl(field_type, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_INSTANCE:
        case XR_KIND_CLASS:
        case XR_KIND_INTERFACE:
            if (xr_type_contains_error_impl(type->instance.superclass, depth + 1))
                return true;
            for (int i = 0; i < type->instance.type_arg_count; i++) {
                XrType *arg = type->instance.type_args ? type->instance.type_args[i] : NULL;
                if (xr_type_contains_error_impl(arg, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_FUNCTION:
            for (int i = 0; i < type->function.param_count; i++) {
                XrType *param_type = type->function.params ? type->function.params[i].type : NULL;
                if (xr_type_contains_error_impl(param_type, depth + 1))
                    return true;
            }
            return xr_type_contains_error_impl(type->function.return_type, depth + 1);
        case XR_KIND_TYPE_PARAM:
            return xr_type_contains_error_impl(type->type_param.constraint, depth + 1);
        case XR_KIND_TUPLE:
            for (int i = 0; i < type->tuple.element_count; i++) {
                XrType *element_type =
                    type->tuple.element_types ? type->tuple.element_types[i] : NULL;
                if (xr_type_contains_error_impl(element_type, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_UNION:
            for (uint8_t i = 0; i < type->union_type.member_count; i++) {
                XrType *member = type->union_type.members ? type->union_type.members[i] : NULL;
                if (xr_type_contains_error_impl(member, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_ENUM:
            for (int i = 0; i < type->enum_type.type_arg_count; i++) {
                XrType *arg = type->enum_type.type_args ? type->enum_type.type_args[i] : NULL;
                if (xr_type_contains_error_impl(arg, depth + 1))
                    return true;
            }
            return false;
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_STRING:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
        case XR_KIND_UNKNOWN:
        case XR_KIND_ERROR:
        case XR_KIND_NEVER:
        case XR_KIND_UNIT:
        case XR_KIND_RUNE:
        case XR_KIND_COUNT:
            return false;
    }
    return false;
}

bool xr_type_contains_error(const XrType *type) {
    return xr_type_contains_error_impl(type, 0);
}

// Type parameter (for generics)
XrType *xr_type_new_type_param(XrVMRuntime *X, const char *name, int id) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_TYPE_PARAM);
    if (!type)
        return NULL;
    XrTypePool *pool = resolve_type_pool(X);
    type->type_param.name = name ? xr_pool_strdup(pool, name) : NULL;
    type->type_param.id = id;
    type->type_param.constraint = NULL;
    return type;
}

XrType *xr_type_new_type_param_constrained(XrVMRuntime *X, const char *name, int id,
                                           XrType *constraint) {
    XrType *type = xr_type_new_type_param(X, name, id);
    if (type) {
        type->type_param.constraint = constraint;
    }
    return type;
}

XrType *xr_type_new_class(XrVMRuntime *X, const char *class_name) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_CLASS);
    if (type && class_name) {
        XrTypePool *pool = resolve_type_pool(X);
        type->instance.class_name = xr_pool_strdup(pool, class_name);
    }
    return type;
}

XrType *xr_type_new_interface(XrVMRuntime *X, const char *interface_name) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INTERFACE);
    if (type && interface_name) {
        XrTypePool *pool = resolve_type_pool(X);
        type->instance.class_name = xr_pool_strdup(pool, interface_name);
    }
    return type;
}

// Parameterized interface: e.g. Iterable<int> or Pair<string, User>.
// Mirrors xr_type_new_generic_instance: stores resolved type arguments
// alongside the interface name so constraint checks can compare them
// structurally instead of falling back to bare-name matching.
XrType *xr_type_new_generic_interface(XrVMRuntime *X, const char *interface_name,
                                      XrType **type_args, int type_arg_count) {
    if (type_arg_count < 0)
        return NULL;
    if (type_arg_count > 0 && !type_args)
        return NULL;
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INTERFACE);
    if (!type)
        return NULL;
    XrTypePool *pool = resolve_type_pool(X);

    type->instance.class_name = interface_name ? xr_pool_strdup(pool, interface_name) : NULL;
    type->instance.class_ref = NULL;
    type->instance.superclass = NULL;

    if (type_arg_count > 0) {
        type->instance.type_args =
            (XrType **) type_alloc_array(pool, sizeof(XrType *), type_arg_count, NULL);
        if (!type->instance.type_args)
            return NULL;
        type->instance.type_arg_count = type_arg_count;
        for (int i = 0; i < type_arg_count; i++) {
            type->instance.type_args[i] = type_args[i];
        }
    } else {
        type->instance.type_args = NULL;
        type->instance.type_arg_count = 0;
    }

    return type;
}

XrType *xr_type_new_bigint(XrVMRuntime *X) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = "BigInt";
    return type;
}

XrType *xr_type_new_u8_array(XrVMRuntime *X) {
    XrType *elem = xr_type_new_int_width(X, XR_NATIVE_U8);
    if (!elem)
        return NULL;
    return xr_type_new_array(X, elem);
}

XrType *xr_type_new_u8_slice(XrVMRuntime *X) {
    XrType *elem = xr_type_new_int_width(X, XR_NATIVE_U8);
    if (!elem)
        return NULL;
    return xr_type_new_slice(X, elem);
}

XrType *xr_type_new_regex(XrVMRuntime *X) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = "Regex";
    return type;
}

XrType *xr_type_new_stringbuilder(XrVMRuntime *X) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = "StringBuilder";
    return type;
}

XrType *xr_type_new_named_instance(XrVMRuntime *X, const char *name) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type && name) {
        XrTypePool *pool = resolve_type_pool(X);
        type->instance.class_name = xr_pool_strdup(pool, name);
    }
    return type;
}

XrType *xr_type_new_enum(XrVMRuntime *X, const char *enum_name) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_ENUM);
    if (type && enum_name) {
        XrTypePool *pool = resolve_type_pool(X);
        type->enum_type.enum_name = xr_pool_strdup(pool, enum_name);
        type->enum_type.layout_id = 0;
        type->enum_type.layout = NULL;
        type->enum_type.type_args = NULL;
        type->enum_type.type_arg_count = 0;
    }
    return type;
}

XrType *xr_type_new_generic_enum(XrVMRuntime *X, const char *enum_name, const XrEnumLayout *layout,
                                 XrType **type_args, int type_arg_count) {
    if (type_arg_count < 0 || (type_arg_count > 0 && !type_args))
        return NULL;
    XrType *type = xr_type_new_enum(X, enum_name);
    if (!type)
        return NULL;
    type->enum_type.layout = layout;
    type->enum_type.layout_id = layout ? layout->layout_id : 0;
    if (type_arg_count > 0) {
        XrTypePool *pool = resolve_type_pool(resolve_isolate(X));
        type->enum_type.type_args =
            (XrType **) type_alloc_array(pool, sizeof(XrType *), (size_t) type_arg_count, NULL);
        if (!type->enum_type.type_args)
            return NULL;
        type->enum_type.type_arg_count = type_arg_count;
        for (int i = 0; i < type_arg_count; i++)
            type->enum_type.type_args[i] = type_args[i];
    }
    return type;
}

XrType *xr_type_new_instance(XrVMRuntime *X, XrClassInfo *class_info) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (!type)
        return NULL;
    type->instance.class_ref = class_info;
    // Extract class_name from class_info for type comparison and display
    if (class_info && class_info->name) {
        XrTypePool *pool = resolve_type_pool(X);
        type->instance.class_name = xr_pool_strdup(pool, class_info->name);
    }
    type->instance.type_args = NULL;
    type->instance.type_arg_count = 0;
    return type;
}

XrType *xr_type_new_generic_instance(XrVMRuntime *X, const char *class_name,
                                     XrClassInfo *class_info, XrType **type_args,
                                     int type_arg_count) {
    if (type_arg_count < 0)
        return NULL;
    if (type_arg_count > 0 && !type_args)
        return NULL;
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (!type)
        return NULL;
    XrTypePool *pool = resolve_type_pool(X);

    type->instance.class_name = class_name ? xr_pool_strdup(pool, class_name) : NULL;
    type->instance.class_ref = class_info;
    type->instance.superclass = NULL;

    if (type_arg_count > 0 && type_args) {
        type->instance.type_args =
            (XrType **) type_alloc_array(pool, sizeof(XrType *), type_arg_count, NULL);
        if (!type->instance.type_args)
            return NULL;
        type->instance.type_arg_count = type_arg_count;
        for (int i = 0; i < type_arg_count; i++) {
            type->instance.type_args[i] = type_args[i];
        }
    } else {
        type->instance.type_args = NULL;
        type->instance.type_arg_count = 0;
    }

    return type;
}

XrType *xr_type_new_enum_metadata(XrVMRuntime *X, const char *metadata_name, XrType *enum_type) {
    if (!metadata_name || !enum_type || enum_type->kind != XR_KIND_ENUM ||
        !enum_type->enum_type.layout)
        return NULL;
    XrType *args[1] = {enum_type};
    return xr_type_new_generic_instance(X, metadata_name, NULL, args, 1);
}

// Function type
XrType *xr_type_new_function(XrVMRuntime *X, XrType **param_types, int param_count,
                             XrType *return_type, bool is_variadic) {
    if (param_count < 0)
        return NULL;
    if (param_count > 0 && !param_types)
        return NULL;
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_FUNCTION);
    if (!type)
        return NULL;
    XrTypePool *pool = resolve_type_pool(X);

    if (param_count > 0 && param_types) {
        size_t param_size;
        type->function.params = (XrFunctionParam *) type_alloc_array(pool, sizeof(XrFunctionParam),
                                                                     param_count, &param_size);
        if (!type->function.params)
            return NULL;
        for (int i = 0; i < param_count; i++) {
            type->function.params[i].type = param_types[i];
            type->function.params[i].mode = XR_PARAM_READ;
        }
    }
    type->function.param_count = param_count;
    type->function.min_params = param_count;  // Default: all params required
    type->function.return_type = return_type;
    type->function.is_variadic = is_variadic;
    type->function.receiver_mode = XR_PARAM_READ;
    // Fail-closed default (task 216): a function type is assumed to possibly
    // throw until the analyzer proves otherwise after the effect-DB fixpoint.
    type->function.throw_effect = XR_FN_EFFECT_MAY_THROW;
    type->function.view_return_source = XR_VIEW_RETURN_NONE;
    type->function.view_return_param = -1;
    type->function.view_return_complete = true;
    if (return_type && XR_TYPE_IS_SLICE(return_type)) {
        int borrowed_candidate = -1;
        bool multiple = false;
        for (int i = 0; i < param_count; i++) {
            XrType *param = param_types ? param_types[i] : NULL;
            if (!param || !XR_TYPE_IS_SLICE(param))
                continue;
            if (borrowed_candidate >= 0) {
                multiple = true;
                break;
            }
            borrowed_candidate = i;
        }
        if (multiple) {
            type->function.view_return_source = XR_VIEW_RETURN_MULTI;
            type->function.view_return_complete = false;
        } else if (borrowed_candidate >= 0) {
            type->function.view_return_source = XR_VIEW_RETURN_PARAM;
            type->function.view_return_param = (int16_t) borrowed_candidate;
        } else {
            type->function.view_return_source = XR_VIEW_RETURN_UNKNOWN;
            type->function.view_return_complete = false;
        }
    }
    return type;
}

XR_FUNC void xr_type_set_function_type_params(XrVMRuntime *X, XrType *type, const char **names,
                                              XrType ***constraint_lists,
                                              const int *constraint_counts, int count) {
    if (!type || type->kind != XR_KIND_FUNCTION || count <= 0 || !names)
        return;
    X = resolve_isolate(X);
    XrTypePool *pool = resolve_type_pool(X);
    if (!pool)
        return;
    type->function.type_param_names =
        (const char **) type_alloc_array(pool, sizeof(const char *), count, NULL);
    type->function.type_param_constraints =
        (XrType ***) type_alloc_array(pool, sizeof(XrType **), count, NULL);
    type->function.type_param_constraint_counts =
        (int *) type_alloc_array(pool, sizeof(int), count, NULL);
    if (!type->function.type_param_names || !type->function.type_param_constraints ||
        !type->function.type_param_constraint_counts) {
        type->function.type_param_count = 0;
        return;
    }

    type->function.type_param_count = count;
    for (int i = 0; i < count; i++) {
        type->function.type_param_names[i] = names[i] ? xr_pool_strdup(pool, names[i]) : NULL;
        int n = constraint_counts ? constraint_counts[i] : 0;
        XrType **src = constraint_lists ? constraint_lists[i] : NULL;
        type->function.type_param_constraint_counts[i] = n;
        if (n > 0 && src) {
            type->function.type_param_constraints[i] =
                (XrType **) type_alloc_array(pool, sizeof(XrType *), n, NULL);
            if (!type->function.type_param_constraints[i]) {
                type->function.type_param_constraint_counts[i] = 0;
                continue;
            }
            for (int j = 0; j < n; j++)
                type->function.type_param_constraints[i][j] = src[j];
        } else {
            type->function.type_param_constraints[i] = NULL;
            type->function.type_param_constraint_counts[i] = 0;
        }
    }
}

// Tuple type (for multi-value return, compile-time only)
XrType *xr_type_new_tuple(XrVMRuntime *X, XrType **element_types, int count) {
    X = resolve_isolate(X);
    if (count < 0)
        return NULL;
    if (count == 0)
        return xr_type_new_unit(X);
    if (!element_types)
        return NULL;
    /* Unary tuple `(T,)` is intentionally a distinct type from T:
     * generic / macro contexts need the wrapper to be observable,
     * and the user spelling carries an explicit trailing comma. */

    XrType *type = type_alloc(X, XR_KIND_TUPLE);
    if (!type)
        return NULL;
    XrTypePool *pool = resolve_type_pool(X);

    if (count > 0 && element_types) {
        size_t element_size;
        type->tuple.element_types =
            (XrType **) type_alloc_array(pool, sizeof(XrType *), count, &element_size);
        if (!type->tuple.element_types)
            return NULL;
        memcpy(type->tuple.element_types, element_types, element_size);
    }
    type->tuple.element_count = count;
    return type;
}

int xr_type_tuple_count(XrType *type) {
    if (!type || !XR_TYPE_IS_TUPLE(type))
        return 0;
    return type->tuple.element_count;
}

XrType *xr_type_tuple_get(XrType *type, int index) {
    if (!type || !XR_TYPE_IS_TUPLE(type))
        return NULL;
    if (index < 0 || index >= type->tuple.element_count)
        return NULL;
    return type->tuple.element_types[index];
}

/* ========== Union Type Implementation ========== */

// Helper: sort members by kind for canonical order (insertion sort, count <= 6)
static void union_sort_by_kind(XrType **members, int count) {
    for (int i = 1; i < count; i++) {
        XrType *key = members[i];
        int j = i - 1;
        while (j >= 0 && members[j]->kind > key->kind) {
            members[j + 1] = members[j];
            j--;
        }
        members[j + 1] = key;
    }
}

// Helper: normalize members (dedup, remove never, apply special rules)
// Returns final count. If any member is `any`, returns -1 (caller should return any).
static int union_normalize(XrType **in, int in_count, XrType **out, bool *out_nullable) {
    bool has_null = false;
    int out_count = 0;

    for (int i = 0; i < in_count; i++) {
        XrType *t = in[i];
        if (!t || XR_TYPE_IS_NEVER(t))
            continue;
        if (XR_TYPE_IS_UNKNOWN(t))
            return -1;
        if (XR_TYPE_IS_NULL(t)) {
            has_null = true;
            continue;
        }
        if (t->is_nullable) {
            has_null = true;
        }
        // Dedup by xr_type_equals
        bool dup = false;
        for (int j = 0; j < out_count; j++) {
            if (xr_type_equals(out[j], t)) {
                dup = true;
                break;
            }
        }
        if (!dup && out_count < XR_UNION_MAX_MEMBERS * 2)
            out[out_count++] = t;
    }

    *out_nullable = has_null;
    return out_count;
}

// Construct a union type from an array of member types.
// Applies: flatten existing unions, dedup, sort, special rules.
// Union aliases as members are flattened (from xr_type_union merge path).
XrType *xr_type_new_union(XrVMRuntime *X, XrType **members, int count) {
    X = resolve_isolate(X);
    XR_DCHECK(count >= 0, "type_new_union: negative count");
    XR_DCHECK(count == 0 || members != NULL, "type_new_union: NULL members with count > 0");
    if (count == 0)
        return xr_type_new_never(X);
    if (count == 1)
        return members[0];

    // 1. Flatten any existing union members (from xr_type_union merge)
    XrType *flat[XR_UNION_MAX_MEMBERS * 2];
    int flat_count = 0;
    for (int i = 0; i < count && flat_count < XR_UNION_MAX_MEMBERS * 2; i++) {
        if (!members[i])
            continue;
        if (XR_TYPE_IS_UNION(members[i])) {
            for (int j = 0; j < members[i]->union_type.member_count; j++) {
                if (flat_count < XR_UNION_MAX_MEMBERS * 2)
                    flat[flat_count++] = members[i]->union_type.members[j];
            }
        } else {
            flat[flat_count++] = members[i];
        }
    }

    // 2. Normalize: dedup, remove never, apply special rules
    XrType *result[XR_UNION_MAX_MEMBERS * 2];
    bool has_null = false;
    int result_count = union_normalize(flat, flat_count, result, &has_null);

    // unknown fallback
    if (result_count < 0)
        return xr_type_new_unknown(X);

    // Degenerate: empty -> never
    if (result_count == 0) {
        return has_null ? xr_type_new_null(X) : xr_type_new_never(X);
    }

    // Single member
    if (result_count == 1) {
        if (has_null)
            return xr_type_make_nullable(X, result[0]);
        return result[0];
    }

    // Exceeds max -> degrade to unknown
    if (result_count > XR_UNION_MAX_MEMBERS)
        return xr_type_new_unknown(X);

    // 3. Sort by kind for canonical order
    union_sort_by_kind(result, result_count);

    // 4. Allocate union type
    XrType *type = type_alloc(X, XR_KIND_UNION);
    if (!type)
        return xr_type_new_unknown(X);
    XrTypePool *pool = resolve_type_pool(X);
    size_t member_size;
    type->union_type.members =
        (XrType **) type_alloc_array(pool, sizeof(XrType *), result_count, &member_size);
    if (!type->union_type.members)
        return xr_type_new_unknown(X);
    memcpy(type->union_type.members, result, member_size);
    type->union_type.member_count = (uint8_t) result_count;
    if (has_null)
        type->is_nullable = true;
    return type;
}

// Merge two types into a union (or apply special rules).
// Numeric members remain distinct; only T|null receives nullable sugar.
XrType *xr_type_union(XrVMRuntime *X, XrType *a, XrType *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (xr_type_equals(a, b))
        return a;

    // ErrorType is compiler recovery poison; unioning it stays poison.
    if (XR_TYPE_IS_ERROR(a) || XR_TYPE_IS_ERROR(b))
        return xr_type_new_error(X);

    // unknown | T = unknown
    if (XR_TYPE_IS_UNKNOWN(a) || XR_TYPE_IS_UNKNOWN(b))
        return xr_type_new_unknown(X);

    // never | T = T
    if (XR_TYPE_IS_NEVER(a))
        return b;
    if (XR_TYPE_IS_NEVER(b))
        return a;

    // T | null = T? (nullable shortcut)
    if (XR_TYPE_IS_NULL(a) && !XR_TYPE_IS_NULL(b))
        return xr_type_make_nullable(X, b);
    if (XR_TYPE_IS_NULL(b) && !XR_TYPE_IS_NULL(a))
        return xr_type_make_nullable(X, a);

    // T | T? = T?. Nullability decorates a base type instead of being a union
    // member, so the narrowed and unnarrowed forms of one type must collapse
    // rather than produce the malformed union `T | T?`. Control-flow joins
    // (a narrowed branch merging with a nullable one) hit this constantly.
    if (a->is_nullable != b->is_nullable) {
        XrType *base_a = xr_type_non_nullable(X, a);
        XrType *base_b = xr_type_non_nullable(X, b);
        if (base_a && base_b && xr_type_equals(base_a, base_b))
            return a->is_nullable ? a : b;
    }

    // General case: create real union
    XrType *pair[2] = {a, b};
    return xr_type_new_union(X, pair, 2);
}

// Union accessor: number of members (0 if not a union)
int xr_type_union_count(XrType *type) {
    if (!type || !XR_TYPE_IS_UNION(type))
        return 0;
    return type->union_type.member_count;
}

// Union accessor: get member by index (NULL if out of range)
XrType *xr_type_union_member(XrType *type, int index) {
    if (!type || !XR_TYPE_IS_UNION(type))
        return NULL;
    if (index < 0 || index >= type->union_type.member_count)
        return NULL;
    return type->union_type.members[index];
}

// Check if union contains a member with given kind
bool xr_type_union_contains(XrType *type, XrTypeKind kind) {
    if (!type || !XR_TYPE_IS_UNION(type))
        return type && type->kind == kind;
    for (int i = 0; i < type->union_type.member_count; i++) {
        if (type->union_type.members[i]->kind == kind)
            return true;
    }
    return false;
}

bool xr_type_union_indiscriminable_pair(const XrType *type, XrType **out_first,
                                        XrType **out_second) {
    if (!type || !XR_TYPE_IS_UNION(type))
        return false;
    for (int i = 0; i < type->union_type.member_count; i++) {
        XrType *a = type->union_type.members[i];
        if (!a || (a->kind != XR_KIND_INT && a->kind != XR_KIND_FLOAT))
            continue;
        for (int j = i + 1; j < type->union_type.member_count; j++) {
            XrType *b = type->union_type.members[j];
            if (!b || b->kind != a->kind)
                continue;
            if (out_first)
                *out_first = a;
            if (out_second)
                *out_second = b;
            return true;
        }
    }
    return false;
}

XrType *xr_type_union_numeric_member_for_literal(XrType *type, XrTypeKind literal_kind) {
    if (!type || !XR_TYPE_IS_UNION(type) || type->is_const)
        return NULL;
    if (literal_kind != XR_KIND_INT && literal_kind != XR_KIND_FLOAT)
        return NULL;
    XrType *integer_member = NULL;
    XrType *float_member = NULL;
    for (int i = 0; i < type->union_type.member_count; i++) {
        XrType *member = type->union_type.members[i];
        if (!member || member->is_nullable)
            continue;
        if (member->kind == XR_KIND_INT && !integer_member)
            integer_member = member;
        else if (member->kind == XR_KIND_FLOAT && !float_member)
            float_member = member;
    }
    if (literal_kind == XR_KIND_FLOAT)
        return float_member;
    return integer_member ? integer_member : float_member;
}

// Remove all members with given kind from union; return simplified type
XrType *xr_type_union_remove(XrVMRuntime *X, XrType *type, XrTypeKind kind) {
    if (!type)
        return NULL;
    if (!XR_TYPE_IS_UNION(type)) {
        return type->kind == kind ? xr_type_new_never(X) : type;
    }
    XrType *kept[XR_UNION_MAX_MEMBERS];
    int kept_count = 0;
    for (int i = 0; i < type->union_type.member_count; i++) {
        if (type->union_type.members[i]->kind != kind)
            kept[kept_count++] = type->union_type.members[i];
    }
    if (kept_count == 0)
        return xr_type_new_never(X);
    if (kept_count == 1) {
        XrType *r = kept[0];
        if (type->is_nullable)
            r = xr_type_make_nullable(X, r);
        return r;
    }
    XrType *r = xr_type_new_union(X, kept, kept_count);
    if (type->is_nullable && r)
        r->is_nullable = true;
    return r;
}

// Keep only members with given kind (for typeof narrowing)
static XrType *xr_type_union_filter(XrType *type, XrTypeKind kind) {
    if (!type)
        return NULL;
    if (!XR_TYPE_IS_UNION(type)) {
        return type->kind == kind ? type : xr_type_new_never(NULL);
    }
    for (int i = 0; i < type->union_type.member_count; i++) {
        if (type->union_type.members[i]->kind == kind)
            return type->union_type.members[i];
    }
    return xr_type_new_never(NULL);
}

// Fixed-length array ([T; N] - compile-time length)
XrType *xr_type_new_fixed_array(XrVMRuntime *X, XrType *element_type, int length) {
    XrType *type = type_alloc(X, XR_KIND_FIXED_ARRAY);
    if (!type)
        return NULL;
    type->fixed_array.element_type = element_type;
    type->fixed_array.length = length;
    return type;
}

// Copy a type
XrType *xr_type_copy(XrVMRuntime *X, XrType *type) {
    if (!type)
        return NULL;
    XR_DCHECK(type->kind < XR_KIND_COUNT, "type_copy: invalid kind");
    XrType *copy = type_alloc(X, type->kind);
    if (!copy)
        return NULL;

    XrTypePool *pool = resolve_type_pool(X);

    copy->semantic_type_id = type->semantic_type_id;
    copy->is_nullable = type->is_nullable;

    copy->is_const = type->is_const;
    copy->scalar_rep = type->scalar_rep;

    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
        case XR_KIND_POINTER:
            copy->container.element_type = type->container.element_type;
            copy->ptr_is_mut = type->ptr_is_mut;  // harmless for non-pointer container kinds
            break;
        case XR_KIND_MAP:
            copy->map.key_type = type->map.key_type;
            copy->map.value_type = type->map.value_type;
            break;
        case XR_KIND_ENUM:
            copy->enum_type.enum_name =
                type->enum_type.enum_name ? xr_pool_strdup(pool, type->enum_type.enum_name) : NULL;
            copy->enum_type.layout_id = type->enum_type.layout_id;
            copy->enum_type.layout = type->enum_type.layout;
            copy->enum_type.type_arg_count = type->enum_type.type_arg_count;
            if (type->enum_type.type_arg_count > 0 && type->enum_type.type_args) {
                copy->enum_type.type_args = (XrType **) type_alloc_array(
                    pool, sizeof(XrType *), (size_t) type->enum_type.type_arg_count, NULL);
                if (!copy->enum_type.type_args)
                    return NULL;
                for (int i = 0; i < type->enum_type.type_arg_count; i++)
                    copy->enum_type.type_args[i] = type->enum_type.type_args[i];
            }
            break;
        case XR_KIND_INSTANCE:
        case XR_KIND_CLASS:
            if (type->instance.type_arg_count < 0)
                return NULL;
            copy->instance.class_name =
                type->instance.class_name ? xr_pool_strdup(pool, type->instance.class_name) : NULL;
            copy->instance.class_ref = type->instance.class_ref;
            copy->instance.superclass = type->instance.superclass;
            if (type->instance.type_arg_count > 0 && type->instance.type_args) {
                size_t type_arg_size;
                copy->instance.type_args = (XrType **) type_alloc_array(
                    pool, sizeof(XrType *), type->instance.type_arg_count, &type_arg_size);
                if (!copy->instance.type_args)
                    return NULL;
                memcpy(copy->instance.type_args, type->instance.type_args, type_arg_size);
                copy->instance.type_arg_count = type->instance.type_arg_count;
            } else {
                copy->instance.type_args = NULL;
                copy->instance.type_arg_count = 0;
            }
            break;
        case XR_KIND_FUNCTION:
            if (type->function.param_count < 0)
                return NULL;
            if (type->function.param_count > 0 && !type->function.params)
                return NULL;
            if (type->function.param_count > 0) {
                size_t param_size;
                copy->function.params = (XrFunctionParam *) type_alloc_array(
                    pool, sizeof(XrFunctionParam), type->function.param_count, &param_size);
                if (!copy->function.params)
                    return NULL;
                memcpy(copy->function.params, type->function.params, param_size);
            }
            copy->function.param_count = type->function.param_count;
            copy->function.min_params = type->function.min_params;
            copy->function.return_type = type->function.return_type;
            copy->function.is_variadic = type->function.is_variadic;
            copy->function.is_c_abi = type->function.is_c_abi;
            copy->function.receiver_mode = type->function.receiver_mode;
            copy->function.throw_effect = type->function.throw_effect;  // task 216
            copy->function.view_return_source = type->function.view_return_source;
            copy->function.view_return_param = type->function.view_return_param;
            copy->function.view_return_complete = type->function.view_return_complete;
            if (type->function.type_param_count > 0 && type->function.type_param_names) {
                xr_type_set_function_type_params(
                    X, copy, type->function.type_param_names, type->function.type_param_constraints,
                    type->function.type_param_constraint_counts, type->function.type_param_count);
            }
            break;
        case XR_KIND_STRUCT_OBJECT:
        case XR_KIND_JSON:
            if (type->object.field_count < 0)
                return NULL;
            if (type->object.field_count > 0) {
                copy->object.field_count = type->object.field_count;
                copy->object.type_name =
                    type->object.type_name ? xr_pool_strdup(pool, type->object.type_name) : NULL;
                if (type->object.field_names) {
                    size_t field_name_size;
                    copy->object.field_names = (const char **) type_alloc_array(
                        pool, sizeof(const char *), type->object.field_count, &field_name_size);
                    if (!copy->object.field_names)
                        return NULL;
                    for (int i = 0; i < type->object.field_count; i++) {
                        copy->object.field_names[i] =
                            type->object.field_names[i]
                                ? xr_pool_strdup(pool, type->object.field_names[i])
                                : NULL;
                    }
                }
                if (type->object.field_types) {
                    size_t field_type_size;
                    copy->object.field_types = (XrType **) type_alloc_array(
                        pool, sizeof(XrType *), type->object.field_count, &field_type_size);
                    if (!copy->object.field_types)
                        return NULL;
                    memcpy(copy->object.field_types, type->object.field_types, field_type_size);
                }
                if (type->object.field_readonly) {
                    size_t field_readonly_size;
                    copy->object.field_readonly = (bool *) type_alloc_array(
                        pool, sizeof(bool), type->object.field_count, &field_readonly_size);
                    if (!copy->object.field_readonly)
                        return NULL;
                    memcpy(copy->object.field_readonly, type->object.field_readonly,
                           field_readonly_size);
                }
            }
            break;
        case XR_KIND_TYPE_PARAM:
            copy->type_param.name =
                type->type_param.name ? xr_pool_strdup(pool, type->type_param.name) : NULL;
            copy->type_param.id = type->type_param.id;
            copy->type_param.constraint = type->type_param.constraint;
            break;
        case XR_KIND_UNION:
            copy->union_type.member_count = type->union_type.member_count;
            if (type->union_type.member_count > 0 && type->union_type.members) {
                size_t union_member_size;
                copy->union_type.members = (XrType **) type_alloc_array(
                    pool, sizeof(XrType *), type->union_type.member_count, &union_member_size);
                if (!copy->union_type.members)
                    return NULL;
                memcpy(copy->union_type.members, type->union_type.members, union_member_size);
            } else {
                copy->union_type.members = NULL;
            }
            break;
        case XR_KIND_TUPLE:
            /* Without this branch a copied tuple collapses to arity-0
             * (rendered as `()`), silently breaking every generic
             * substitution that uses a tuple as the actual type
             * — e.g. `Channel<(int, string)>.send(value: T)` ends up
             * with parameter type `()`. */
            copy->tuple.element_count = type->tuple.element_count;
            if (type->tuple.element_count > 0 && type->tuple.element_types) {
                size_t tuple_elem_size;
                copy->tuple.element_types = (XrType **) type_alloc_array(
                    pool, sizeof(XrType *), type->tuple.element_count, &tuple_elem_size);
                if (!copy->tuple.element_types)
                    return NULL;
                memcpy(copy->tuple.element_types, type->tuple.element_types, tuple_elem_size);
            } else {
                copy->tuple.element_types = NULL;
            }
            break;
        case XR_KIND_FIXED_ARRAY:
            copy->fixed_array.element_type = type->fixed_array.element_type;
            copy->fixed_array.length = type->fixed_array.length;
            break;
        default:
            break;
    }

    return copy;
}

// Make a type nullable (returns cached singleton for primitive types)
XrType *xr_type_make_nullable(XrVMRuntime *X, XrType *type) {
    if (!type)
        return NULL;
    if (xr_type_intrinsically_includes_null(type))
        return type;
    if (type->is_nullable)
        return type;
    X = resolve_isolate(X);

    // Fast path: return process-level nullable singletons for common types
    if (type == &g_type_int)
        return &g_type_int_nullable;
    if (type == &g_type_float)
        return &g_type_float_nullable;
    if (type == &g_type_string)
        return &g_type_string_nullable;
    if (type == &g_type_bool)
        return &g_type_bool_nullable;
    if (type == &g_type_rune)
        return &g_type_rune_nullable;

    // Also handle pool singletons (frozen types from type pool)
    if (type->frozen && resolve_type_pool(X)) {
        switch (type->kind) {
            case XR_KIND_INT:
                if (type->scalar_rep != XR_NATIVE_I64)
                    break;
                return &g_type_int_nullable;
            case XR_KIND_FLOAT:
                if (type->scalar_rep != XR_NATIVE_F64)
                    break;
                return &g_type_float_nullable;
            case XR_KIND_STRING:
                return &g_type_string_nullable;
            case XR_KIND_BOOL:
                return &g_type_bool_nullable;
            case XR_KIND_RUNE:
                return &g_type_rune_nullable;
            default:
                break;
        }
    }

    /* Qualifiers are values, not mutations of declaration identity.  Named
     * enum/class/structural-object types are shared through analyzer symbol links even
     * before they are frozen; mutating one while resolving `T?` would silently
     * make the exported declaration itself nullable.  Mirror
     * xr_type_make_const(): every non-singleton qualifier gets its own copy. */
    XrType *copy = xr_type_copy(X, type);
    if (copy)
        copy->is_nullable = true;
    return copy;
}

// Check if source type is assignable to target type
// This is the core type compatibility check, migrated from sema/ct_compatible
static int xr_numeric_scalar_width(uint8_t scalar_rep) {
    switch ((XrNativeType) scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
            return 8;
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
            return 16;
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_F32:
            return 32;
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_F64:
            return 64;
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return 0; /* Target dependent: never implicitly mix with fixed width. */
        default:
            return -1;
    }
}

XR_FUNC const char *xr_conversion_kind_name(XrConversionKind kind) {
    switch (kind) {
        case XR_CONVERSION_NONE:
            return "none";
        case XR_CONVERSION_IDENTITY:
            return "identity";
        case XR_CONVERSION_CONTEXTUAL_LITERAL:
            return "contextual_literal";
        case XR_CONVERSION_LOSSLESS_WIDEN:
            return "lossless_widen";
        case XR_CONVERSION_EXPLICIT_TRUNCATE:
            return "explicit_truncate";
        case XR_CONVERSION_EXPLICIT_SIGN_CHANGE:
            return "explicit_sign_change";
        case XR_CONVERSION_EXPLICIT_TARGET_WIDTH:
            return "explicit_target_width";
        case XR_CONVERSION_EXPLICIT_INT_FLOAT:
            return "explicit_int_float";
        case XR_CONVERSION_ENUM_ORDINAL:
            return "enum_ordinal";
        case XR_CONVERSION_DYNAMIC_CHECKED:
            return "dynamic_checked";
        case XR_CONVERSION_DYNAMIC_NULLABLE:
            return "dynamic_nullable";
        case XR_CONVERSION_DISALLOWED:
            return "disallowed";
        default:
            return "invalid";
    }
}

XrConversionKind xr_type_numeric_conversion_kind(const XrType *target, const XrType *source) {
    if (!target || !source || !XR_TYPE_IS_NUMERIC(target) || !XR_TYPE_IS_NUMERIC(source) ||
        target->is_nullable || source->is_nullable)
        return XR_CONVERSION_DISALLOWED;
    if (xr_type_equals((XrType *) target, (XrType *) source))
        return XR_CONVERSION_IDENTITY;
    if (XR_TYPE_IS_INT(target) != XR_TYPE_IS_INT(source))
        return XR_CONVERSION_EXPLICIT_INT_FLOAT;

    if (XR_TYPE_IS_FLOAT(target)) {
        int target_width = xr_numeric_scalar_width(target->scalar_rep);
        int source_width = xr_numeric_scalar_width(source->scalar_rep);
        return target_width > source_width ? XR_CONVERSION_LOSSLESS_WIDEN
                                           : XR_CONVERSION_EXPLICIT_TRUNCATE;
    }

    bool target_sized =
        target->scalar_rep == XR_NATIVE_ISIZE || target->scalar_rep == XR_NATIVE_USIZE;
    bool source_sized =
        source->scalar_rep == XR_NATIVE_ISIZE || source->scalar_rep == XR_NATIVE_USIZE;
    if (target_sized || source_sized)
        return XR_CONVERSION_EXPLICIT_TARGET_WIDTH;
    if (xr_scalar_rep_is_unsigned(target->scalar_rep) !=
        xr_scalar_rep_is_unsigned(source->scalar_rep))
        return XR_CONVERSION_EXPLICIT_SIGN_CHANGE;

    int target_width = xr_numeric_scalar_width(target->scalar_rep);
    int source_width = xr_numeric_scalar_width(source->scalar_rep);
    return target_width > source_width ? XR_CONVERSION_LOSSLESS_WIDEN
                                       : XR_CONVERSION_EXPLICIT_TRUNCATE;
}

bool xr_type_numeric_implicitly_convertible(const XrType *target, const XrType *source) {
    XrConversionKind kind = xr_type_numeric_conversion_kind(target, source);
    return kind == XR_CONVERSION_IDENTITY || kind == XR_CONVERSION_LOSSLESS_WIDEN;
}

XrType *xr_type_numeric_common_type(XrType *left, XrType *right) {
    if (!left || !right || !XR_TYPE_IS_NUMERIC(left) || !XR_TYPE_IS_NUMERIC(right))
        return NULL;
    if (xr_type_numeric_implicitly_convertible(left, right))
        return left;
    if (xr_type_numeric_implicitly_convertible(right, left))
        return right;
    return NULL;
}

bool xr_type_assignable(XrType *target, XrType *source) {
    if (!target || !source)
        return false;

    // Same pointer = same type
    if (target == source)
        return true;

    // Unknown means analysis did not produce a precise type. Keep compatibility
    // permissive here so later diagnostics and IDE queries can continue after
    // earlier errors.
    if (XR_TYPE_IS_UNKNOWN(target) || XR_TYPE_IS_UNKNOWN(source))
        return true;

    // never is assignable to anything (bottom type)
    if (XR_TYPE_IS_NEVER(source))
        return true;

    /* Deep-readonly authority is directional.  A mutable value can be read
     * through a const target, but a const source must never be stored in a
     * mutable target and thereby regain write authority. */
    if (source->is_const && !target->is_const)
        return false;

    // Type parameter: check constraint if present, otherwise compatible
    if (target->kind == XR_KIND_TYPE_PARAM) {
        if (target->type_param.constraint)
            return xr_type_assignable(target->type_param.constraint, source);
        return true;  // unconstrained T in generic body
    }
    if (source->kind == XR_KIND_TYPE_PARAM) {
        if (source->type_param.constraint)
            return xr_type_assignable(target, source->type_param.constraint);
        return true;  // unconstrained T in generic body
    }

    // Union source: every member must be assignable to target
    if (XR_TYPE_IS_UNION(source)) {
        for (int i = 0; i < source->union_type.member_count; i++) {
            if (!xr_type_assignable(target, source->union_type.members[i]))
                return false;
        }
        return true;
    }

    // Union target: source must be assignable to at least one member
    if (XR_TYPE_IS_UNION(target)) {
        // null → nullable union is always valid
        if (source->kind == XR_KIND_NULL && target->is_nullable)
            return true;
        for (int i = 0; i < target->union_type.member_count; i++) {
            if (xr_type_assignable(target->union_type.members[i], source))
                return true;
        }
        return false;
    }

    // Equal types are compatible
    if (xr_type_equals(target, source))
        return true;

    // Enum / class-name alias: the parser cannot distinguish an enum
    // type annotation `Color` from a class annotation, so it produces
    // XR_KIND_CLASS (or XR_KIND_INSTANCE for generic forms like
    // Result<int, string>) for any user-defined name. The analyzer
    // later produces XR_KIND_ENUM for enum-static and enum-iter
    // expressions. Treat CLASS/INSTANCE(name=X) and ENUM(enum_name=X)
    // as the same type when the names match, regardless of generic
    // type arguments (enums are type-erased at runtime).
    {
        const char *t_name = NULL;
        const char *s_name = NULL;
        if ((target->kind == XR_KIND_CLASS || target->kind == XR_KIND_INSTANCE) &&
            target->instance.class_name)
            t_name = target->instance.class_name;
        else if (target->kind == XR_KIND_ENUM && target->enum_type.enum_name)
            t_name = target->enum_type.enum_name;
        if ((source->kind == XR_KIND_CLASS || source->kind == XR_KIND_INSTANCE) &&
            source->instance.class_name)
            s_name = source->instance.class_name;
        else if (source->kind == XR_KIND_ENUM && source->enum_type.enum_name)
            s_name = source->enum_type.enum_name;
        if (t_name && s_name && strcmp(t_name, s_name) == 0 && target->kind != source->kind) {
            return true;
        }
    }

    // null is compatible with nullable type (T?)
    if (XR_TYPE_IS_NULL(source) && target->is_nullable)
        return true;
    if (XR_TYPE_IS_NULL(source) && xr_type_intrinsically_includes_null(target))
        return true;

    // Nullable target: compare base types (T assignable to T?, T? assignable to T?)
    if (target->is_nullable) {
        XrType *target_base = xr_type_non_nullable(NULL, target);
        XrType *source_base = source->is_nullable ? xr_type_non_nullable(NULL, source) : source;
        if (target_base && source_base)
            return xr_type_assignable(target_base, source_base);
    }

    // T? → T is NOT silently allowed here.
    // Analyzer must check via xa_check_null_safety and require explicit unwrap.
    // A sink that already includes null is the exception: the unwrap exists to
    // stop a null reaching a type that cannot represent one, and Json can. It
    // is the same reason the null-safety analyzer skips such a target, and
    // without it `string?` could not reach a Json sink that plain `null` can.
    if (source->is_nullable && !target->is_nullable &&
        !xr_type_intrinsically_includes_null(target)) {
        return false;
    }

    // Numeric language assignment is intentionally stricter than slot
    // storage. Only value-preserving widening is implicit; narrowing,
    // signedness/target-width changes and int/float conversion require `as`.
    // Nullable structure has already been handled above, so classification
    // always sees the actual scalar types.
    if (XR_TYPE_IS_NUMERIC(target) && XR_TYPE_IS_NUMERIC(source))
        return xr_type_numeric_implicitly_convertible(target, source);

    // JSON.Value admits scalars implicitly. Its own Map/Array arms already
    // carry the domain type; arbitrary composite values require JSON.value.
    if (XR_TYPE_IS_JSON(target)) {
        switch (source->kind) {
            case XR_KIND_JSON:
            case XR_KIND_NULL:
            case XR_KIND_INT:
            case XR_KIND_FLOAT:
            case XR_KIND_STRING:
            case XR_KIND_BOOL:
                return true;
            case XR_KIND_ARRAY:
                /* Array<Json> is the array form of the domain itself, so it
                 * crosses like any other Json value. Every other container
                 * stays out under the rule above -- Array<int> is a typed
                 * container, not a Json array. */
                if (source->container.element_type &&
                    source->container.element_type->kind == XR_KIND_JSON)
                    return true;
                break;
            default:
                break;
        }
    }

    // Exact structural object assignment.
    bool target_is_struct = XR_TYPE_HAS_OBJECT_SHAPE(target);
    bool source_is_struct = XR_TYPE_HAS_OBJECT_SHAPE(source);
    if (target_is_struct && source_is_struct && target->kind == source->kind) {
        if (XR_TYPE_IS_STRUCT_OBJECT(target) &&
            target->object.field_count != source->object.field_count)
            return false;
        // Check structural compatibility (source has all target fields)
        for (int i = 0; i < target->object.field_count; i++) {
            XrType *target_field_type =
                target->object.field_types ? target->object.field_types[i] : NULL;

            bool is_optional = target_field_type && target_field_type->is_nullable;

            bool found = false;
            for (int j = 0; j < source->object.field_count; j++) {
                if (target->object.field_names && source->object.field_names &&
                    target->object.field_names[i] && source->object.field_names[j] &&
                    strcmp(target->object.field_names[i], source->object.field_names[j]) == 0) {
                    if (target_field_type && source->object.field_types) {
                        XrType *source_field_type = source->object.field_types[j];
                        /* A nullable object field is optional when absent, but an
                         * explicitly present null value must still be checked
                         * against the nullable target itself. */
                        if (!xr_type_assignable(target_field_type, source_field_type)) {
                            return false;
                        }
                        /* Object fields are invariant because a mutable alias
                         * may write a declared field. A literal
                         * null is the sole value-level exception: it initializes
                         * an absent nullable slot and does not create a narrower
                         * mutable field contract. Contextual object literals use
                         * the declared field type for every other initialized
                         * slot. */
                        if (!XR_TYPE_IS_NULL(source_field_type) &&
                            !xr_type_assignable(source_field_type, target_field_type)) {
                            return false;
                        }
                        bool target_readonly = target->object.field_readonly
                                                   ? target->object.field_readonly[i]
                                                   : false;
                        bool source_readonly = source->object.field_readonly
                                                   ? source->object.field_readonly[j]
                                                   : false;
                        /* A mutable object may be viewed through a readonly
                         * field capability, but a readonly source must never be
                         * widened to a mutable target. */
                        if (!target_readonly && source_readonly)
                            return false;
                    }
                    found = true;
                    break;
                }
            }
            if (!found && (XR_TYPE_IS_STRUCT_OBJECT(target) || !is_optional)) {
                return false;
            }
        }
        if (!xr_type_object_accepts_extra_fields(target)) {
            for (int j = 0; j < source->object.field_count; j++) {
                if (!source->object.field_names || !source->object.field_names[j])
                    continue;
                bool exists_on_target = false;
                for (int i = 0; i < target->object.field_count; i++) {
                    if (target->object.field_names && target->object.field_names[i] &&
                        strcmp(source->object.field_names[j], target->object.field_names[i]) == 0) {
                        exists_on_target = true;
                        break;
                    }
                }
                if (!exists_on_target) {
                    return false;
                }
            }
        }
        return true;
    }

    // Array type compatibility (invariant, with unknown element fallback)
    if (XR_TYPE_IS_ARRAY(target) && XR_TYPE_IS_ARRAY(source)) {
        if (!target->container.element_type || !source->container.element_type)
            return true;
        // unknown element type: allow covariant (Array<int> -> Array<unknown>)
        if (XR_TYPE_IS_UNKNOWN(target->container.element_type) ||
            XR_TYPE_IS_UNKNOWN(source->container.element_type))
            return true;
        // Invariant: bidirectional assignability (Array<Dog> ≠ Array<Animal>)
        return xr_type_assignable(target->container.element_type, source->container.element_type) &&
               xr_type_assignable(source->container.element_type, target->container.element_type);
    }

    // Slice compatibility is explicit and target-typed. Owner-to-view
    // conversion is produced by slice syntax or another proven view source,
    // not by general assignment.
    if (XR_TYPE_IS_SLICE(target) && XR_TYPE_IS_SLICE(source)) {
        if (!target->container.element_type || !source->container.element_type)
            return true;
        if (XR_TYPE_IS_UNKNOWN(target->container.element_type) ||
            XR_TYPE_IS_UNKNOWN(source->container.element_type))
            return true;
        return xr_type_assignable(target->container.element_type, source->container.element_type) &&
               xr_type_assignable(source->container.element_type, target->container.element_type);
    }

    // Map type compatibility (invariant, with unknown element fallback)
    if (XR_TYPE_IS_MAP(target) && XR_TYPE_IS_MAP(source)) {
        if (!target->map.key_type || !source->map.key_type)
            return true;
        bool key_ok = xr_type_assignable(target->map.key_type, source->map.key_type) &&
                      xr_type_assignable(source->map.key_type, target->map.key_type);
        bool val_ok = !target->map.value_type || !source->map.value_type ||
                      (xr_type_assignable(target->map.value_type, source->map.value_type) &&
                       xr_type_assignable(source->map.value_type, target->map.value_type));
        return key_ok && val_ok;
    }

    // Fixed-length arrays are value-shaped and distinct from dynamic Array<T>.
    if (target->kind == XR_KIND_FIXED_ARRAY && source->kind == XR_KIND_FIXED_ARRAY) {
        if (target->fixed_array.length != source->fixed_array.length)
            return false;
        if (!target->fixed_array.element_type || !source->fixed_array.element_type)
            return true;
        return xr_type_assignable(target->fixed_array.element_type,
                                  source->fixed_array.element_type) &&
               xr_type_assignable(source->fixed_array.element_type,
                                  target->fixed_array.element_type);
    }

    // Tuple covariance: same arity, element-wise assignable. Tuples
    // and arrays are deliberately distinct shapes — no implicit
    // conversion either way (a fixed-arity heterogeneous tuple has
    // nothing to do with a homogeneous resizable Array).
    if (target->kind == XR_KIND_TUPLE && source->kind == XR_KIND_TUPLE) {
        if (target->tuple.element_count != source->tuple.element_count)
            return false;
        for (int i = 0; i < target->tuple.element_count; i++) {
            XrType *te = target->tuple.element_types[i];
            XrType *se = source->tuple.element_types[i];
            if (!te || !se) {
                /* Unknown element type: stay permissive, matches the
                 * fallback used by other container rules above. */
                continue;
            }
            if (!xr_type_assignable(te, se))
                return false;
        }
        return true;
    }

    // Set type compatibility (invariant, with unknown element fallback)
    if (XR_TYPE_IS_SET(target) && XR_TYPE_IS_SET(source)) {
        if (!target->container.element_type || !source->container.element_type)
            return true;
        return xr_type_assignable(target->container.element_type, source->container.element_type) &&
               xr_type_assignable(source->container.element_type, target->container.element_type);
    }

    // Channel type compatibility
    if (target->kind == XR_KIND_CHANNEL && source->kind == XR_KIND_CHANNEL) {
        if (!target->container.element_type || !source->container.element_type)
            return true;
        return xr_type_assignable(target->container.element_type, source->container.element_type);
    }

    // Raw pointer compatibility (FFI). MutPtr<T> is assignable to Ptr<T>
    // (mut -> const), not the reverse; pointee types are invariant. A null
    // raw pointer (Ptr.null()) is modelled as POINTER and assignable either way.
    if (target->kind == XR_KIND_POINTER && source->kind == XR_KIND_POINTER) {
        if (source->ptr_is_mut == false && target->ptr_is_mut == true)
            return false;  // const -> mut is not allowed
        XrType *te = target->container.element_type;
        XrType *se = source->container.element_type;
        if (!te || !se || XR_TYPE_IS_UNKNOWN(te) || XR_TYPE_IS_UNKNOWN(se))
            return true;
        return xr_type_assignable(te, se) && xr_type_assignable(se, te);
    }

    // Task type compatibility: both are the builtin INSTANCE named "Task".
    // A user class of that name is nominal like any other and must not get
    // the builtin's type-argument variance.
    if (xr_type_is_builtin_named_class(target, "Task") &&
        xr_type_is_builtin_named_class(source, "Task")) {
        XrType *tr = (target->instance.type_arg_count > 0) ? target->instance.type_args[0] : NULL;
        XrType *sr = (source->instance.type_arg_count > 0) ? source->instance.type_args[0] : NULL;
        if (!tr || !sr)
            return true;
        return xr_type_assignable(tr, sr);
    }

    // Function type compatibility.
    //
    // Callback arity tolerance: source may declare *fewer* parameters than
    // target. Trailing target parameters are simply ignored at the call
    // site, matching JS/Python/Rust and how the xray VM already invokes
    // such callbacks. This is what lets `arr.map(fn(x) { ... })` satisfy
    // a `fn(item: T, index: int): U` parameter slot. The source must not
    // declare *more* parameters than target, since those would never get
    // a value.
    if (XR_TYPE_IS_FUNCTION(target) && XR_TYPE_IS_FUNCTION(source)) {
        /* A borrowed Slice return is only sound when callers and callees agree
         * on the unique backing source.  This contract is invariant: ordinary
         * callback arity/return covariance must not erase or rewrite it. */
        if (target->function.view_return_source != source->function.view_return_source ||
            target->function.view_return_param != source->function.view_return_param ||
            target->function.view_return_complete != source->function.view_return_complete)
            return false;
        if (target->function.receiver_mode != source->function.receiver_mode)
            return false;
        /* Effect covariance. POLY is an inference variable at this stage: a
         * target POLY accepts either concrete effect, while a source POLY is
         * provisionally accepted and resolved/rechecked after effect fixpoint. */
        if (target->function.throw_effect == XR_FN_EFFECT_NO_THROW &&
            source->function.throw_effect != XR_FN_EFFECT_NO_THROW &&
            source->function.throw_effect != XR_FN_EFFECT_POLY) {
            return false;
        }
        if (target->function.is_c_abi != source->function.is_c_abi) {
            // One-way coercion: an ordinary xray function value may be used where a
            // CFn<...> (first-class C-ABI function item) is expected. The
            // "module-level, non-capturing, exact-signature" requirement is enforced
            // at the value-formation site (analyzer) and at stub emission (AOT),
            // which keeps VM and AOT fail-closed in lock-step. A CFn value is NOT
            // assignable back to an ordinary closure type.
            if (!(target->function.is_c_abi && !source->function.is_c_abi)) {
                return false;
            }
        }
        if (source->function.param_count > target->function.param_count) {
            return false;
        }
        /* Lower bound: the source must accept at least as many parameters as the
         * target is guaranteed to pass. Without it the fewer-parameter case is
         * silently tolerated, so a one-parameter lambda satisfies a
         * two-parameter contract and then reads a missing argument at run time.
         * A contract that admits a shorter callback says so by lowering its own
         * min_params (as the array HOFs do for the optional index). */
        if (source->function.param_count < target->function.min_params) {
            return false;
        }

        // Return type: covariant - source return must be assignable to target return
        // Special case: void target accepts any return type
        if (target->function.return_type && source->function.return_type) {
            if (!XR_TYPE_IS_UNIT(target->function.return_type)) {
                if (!xr_type_assignable(target->function.return_type,
                                        source->function.return_type)) {
                    return false;
                }
            }
        }

        // Parameters: contravariant over the prefix that source declares.
        // Simplified: allow if types match or either side is unknown/type_param.
        for (int i = 0; i < source->function.param_count; i++) {
            if (xr_type_function_param_mode(target, i) != xr_type_function_param_mode(source, i)) {
                return false;
            }

            XrType *t_param = xr_type_function_param_type(target, i);
            XrType *s_param = xr_type_function_param_type(source, i);

            if (!t_param || !s_param)
                continue;
            if (XR_TYPE_IS_UNKNOWN(t_param) || XR_TYPE_IS_UNKNOWN(s_param))
                continue;
            if (t_param->kind == XR_KIND_TYPE_PARAM || s_param->kind == XR_KIND_TYPE_PARAM)
                continue;

            if (!xr_type_assignable(s_param, t_param)) {
                return false;
            }
        }

        return true;
    }

    // Class inheritance + interface conformance
    if ((target->kind == XR_KIND_CLASS || target->kind == XR_KIND_INSTANCE ||
         target->kind == XR_KIND_INTERFACE) &&
        (source->kind == XR_KIND_CLASS || source->kind == XR_KIND_INSTANCE)) {
        if (xr_type_is_subclass_of(source, target))
            return true;

        // Check interface conformance: source class implements target interface.
        // Target may be XR_KIND_INTERFACE (builtin) or XR_KIND_CLASS (user-defined
        // interface resolved by parser as class name).  When the target carries
        // type arguments, both the head name and the parameter list must match;
        // the bare-name path covers built-ins that ignore parameters.
        const char *target_name = target->instance.class_name;
        int target_args = target->instance.type_arg_count;
        if (target_name) {
            for (XrClassInfo *ci = source->instance.class_ref; ci; ci = ci->base) {
                for (int i = 0; i < ci->interface_count; i++) {
                    XrType *iface = ci->interface_types[i];
                    if (!iface || !iface->instance.class_name)
                        continue;
                    if (strcmp(iface->instance.class_name, target_name) != 0)
                        continue;
                    if (target_args == 0)
                        return true;
                    if (iface->instance.type_arg_count != target_args)
                        continue;
                    bool args_match = true;
                    for (int j = 0; j < target_args; j++) {
                        XrType *t = target->instance.type_args[j];
                        XrType *s = iface->instance.type_args[j];
                        if (!t || !s)
                            continue;
                        if (XR_TYPE_IS_UNKNOWN(t) || XR_TYPE_IS_UNKNOWN(s))
                            continue;
                        if (t->kind == XR_KIND_TYPE_PARAM || s->kind == XR_KIND_TYPE_PARAM)
                            continue;
                        if (!xr_type_assignable(t, s)) {
                            args_match = false;
                            break;
                        }
                    }
                    if (args_match)
                        return true;
                }
            }
        }
        return false;
    }

    return false;
}

/* Index of `name` in an object shape's field list, or -1. */
static int object_shape_field_index_by_name(XrType *shape, const char *name) {
    if (!shape || !name || !shape->object.field_names)
        return -1;
    for (int i = 0; i < shape->object.field_count; i++) {
        const char *fn = shape->object.field_names[i];
        if (fn && strcmp(fn, name) == 0)
            return i;
    }
    return -1;
}

bool xr_type_object_mismatch_reason(XrType *target, XrType *source, char *buf, size_t n) {
    if (!target || !source || !buf || n == 0)
        return false;

    /* Peel invariant containers so a width mismatch inside Array/Slice/Map
     * still localizes. Only recurse when the element/value pair is itself the
     * structural cause, so Array<int> vs Array<string> yields no bogus reason. */
    if ((XR_TYPE_IS_ARRAY(target) && XR_TYPE_IS_ARRAY(source)) ||
        (XR_TYPE_IS_SLICE(target) && XR_TYPE_IS_SLICE(source))) {
        char sub[192];
        if (xr_type_object_mismatch_reason(target->container.element_type,
                                           source->container.element_type, sub, sizeof(sub))) {
            snprintf(buf, n, "element: %s", sub);
            return true;
        }
        return false;
    }
    if (XR_TYPE_IS_MAP(target) && XR_TYPE_IS_MAP(source)) {
        char sub[192];
        if (xr_type_object_mismatch_reason(target->map.value_type, source->map.value_type, sub,
                                           sizeof(sub))) {
            snprintf(buf, n, "value: %s", sub);
            return true;
        }
        return false;
    }

    /* Only object shapes of the same kind carry a field-set reason; structural object and
     * Json are distinct domains and never bridge (mirrors xr_type_assignable). */
    if (!XR_TYPE_HAS_OBJECT_SHAPE(target) || !XR_TYPE_HAS_OBJECT_SHAPE(source) ||
        target->kind != source->kind)
        return false;

    /* 1. Extra field an exact target does not declare. */
    if (!xr_type_object_accepts_extra_fields(target) && source->object.field_names) {
        for (int j = 0; j < source->object.field_count; j++) {
            const char *sf = source->object.field_names[j];
            if (sf && object_shape_field_index_by_name(target, sf) < 0) {
                snprintf(buf, n, "extra field '%s'", sf);
                return true;
            }
        }
    }

    /* 2. Required (non-nullable) target field the source omits. */
    if (target->object.field_names) {
        for (int i = 0; i < target->object.field_count; i++) {
            const char *tf = target->object.field_names[i];
            if (!tf)
                continue;
            XrType *tft = target->object.field_types ? target->object.field_types[i] : NULL;
            bool is_optional = tft && tft->is_nullable;
            if (!is_optional && object_shape_field_index_by_name(source, tf) < 0) {
                snprintf(buf, n, "missing field '%s'", tf);
                return true;
            }
        }
    }

    /* 3. First shared field whose type is incompatible; recurse one level so a
     *    nested shape reports its own field, otherwise name the type pair. */
    if (target->object.field_names && target->object.field_types && source->object.field_types) {
        for (int i = 0; i < target->object.field_count; i++) {
            const char *tf = target->object.field_names[i];
            if (!tf)
                continue;
            int j = object_shape_field_index_by_name(source, tf);
            if (j < 0)
                continue;
            XrType *tft = target->object.field_types[i];
            XrType *sft = source->object.field_types[j];
            if (tft && sft && !xr_type_assignable(tft, sft)) {
                char sub[192];
                if (xr_type_object_mismatch_reason(tft, sft, sub, sizeof(sub)))
                    snprintf(buf, n, "field '%s': %s", tf, sub);
                else
                    snprintf(buf, n, "field '%s' has type '%s', expected '%s'", tf,
                             xr_type_to_string(sft), xr_type_to_string(tft));
                return true;
            }
        }
    }

    return false;
}

static bool function_type_params_equal(XrType *a, XrType *b) {
    if (!a || !b || a->kind != XR_KIND_FUNCTION || b->kind != XR_KIND_FUNCTION)
        return false;
    if (a->function.type_param_count != b->function.type_param_count)
        return false;
    for (int i = 0; i < a->function.type_param_count; i++) {
        int ac = a->function.type_param_constraint_counts
                     ? a->function.type_param_constraint_counts[i]
                     : 0;
        int bc = b->function.type_param_constraint_counts
                     ? b->function.type_param_constraint_counts[i]
                     : 0;
        if (ac != bc)
            return false;
        XrType **al =
            a->function.type_param_constraints ? a->function.type_param_constraints[i] : NULL;
        XrType **bl =
            b->function.type_param_constraints ? b->function.type_param_constraints[i] : NULL;
        for (int j = 0; j < ac; j++) {
            XrType *at = al ? al[j] : NULL;
            XrType *bt = bl ? bl[j] : NULL;
            if (!xr_type_equals(at, bt))
                return false;
        }
    }
    return true;
}

bool xr_type_function_signature_assignable(XrType *target, XrType *source) {
    if (!target || !source || target->kind != XR_KIND_FUNCTION || source->kind != XR_KIND_FUNCTION)
        return false;
    /* min_params is not part of signature compatibility: it drives default-value
     * filling at a direct call, not the shape of the function value. A function
     * with defaults (min_params < param_count) may be assigned to the matching
     * function type, and a call through the value supplies every argument --
     * the "defaults apply only to direct calls" rule is enforced separately by
     * symbol kind, not here. Comparing it produced the self-contradictory
     * "X is not assignable to X" diagnostic, since the renderer omits it. */
    if (target->function.param_count != source->function.param_count ||
        target->function.is_variadic != source->function.is_variadic ||
        target->function.is_c_abi != source->function.is_c_abi ||
        target->function.receiver_mode != source->function.receiver_mode ||
        target->function.view_return_source != source->function.view_return_source ||
        target->function.view_return_param != source->function.view_return_param ||
        target->function.view_return_complete != source->function.view_return_complete ||
        !function_type_params_equal(target, source))
        return false;
    if (target->function.throw_effect == XR_FN_EFFECT_NO_THROW &&
        source->function.throw_effect != XR_FN_EFFECT_NO_THROW &&
        source->function.throw_effect != XR_FN_EFFECT_POLY)
        return false;
    if (!xr_type_equals(target->function.return_type, source->function.return_type))
        return false;
    for (int i = 0; i < target->function.param_count; i++) {
        if (xr_type_function_param_mode(target, i) != xr_type_function_param_mode(source, i) ||
            !xr_type_equals(xr_type_function_param_type(target, i),
                            xr_type_function_param_type(source, i)))
            return false;
    }
    return true;
}

// Check if two types are equal
bool xr_type_equals(XrType *a, XrType *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a->kind != b->kind)
        return false;
    if (a->is_nullable != b->is_nullable)
        return false;
    if (a->is_const != b->is_const)
        return false;
    if ((a->kind == XR_KIND_INT || a->kind == XR_KIND_FLOAT) && a->scalar_rep != b->scalar_rep)
        return false;
    if (a->kind == XR_KIND_ERROR)
        return true;

    // Check type-specific data
    if (a->kind == XR_KIND_ARRAY || a->kind == XR_KIND_SLICE || a->kind == XR_KIND_SET ||
        a->kind == XR_KIND_CHANNEL) {
        return xr_type_equals(a->container.element_type, b->container.element_type);
    }
    if (a->kind == XR_KIND_POINTER) {
        return a->ptr_is_mut == b->ptr_is_mut &&
               xr_type_equals(a->container.element_type, b->container.element_type);
    }
    if (a->kind == XR_KIND_MAP) {
        return xr_type_equals(a->map.key_type, b->map.key_type) &&
               xr_type_equals(a->map.value_type, b->map.value_type);
    }
    if (a->kind == XR_KIND_ENUM) {
        /* Enum identity is nominal and generic arguments remain part of the
         * concrete runtime type. Without this branch the trailing return would
         * make unrelated enum declarations and specializations compare equal. */
        if (!a->enum_type.enum_name || !b->enum_type.enum_name ||
            strcmp(a->enum_type.enum_name, b->enum_type.enum_name) != 0 ||
            a->enum_type.type_arg_count != b->enum_type.type_arg_count)
            return false;
        for (int i = 0; i < a->enum_type.type_arg_count; i++) {
            if (!xr_type_equals(a->enum_type.type_args ? a->enum_type.type_args[i] : NULL,
                                b->enum_type.type_args ? b->enum_type.type_args[i] : NULL))
                return false;
        }
        return true;
    }
    if (a->kind == XR_KIND_INSTANCE || a->kind == XR_KIND_CLASS) {
        // Compare class references first
        if (a->instance.class_ref && b->instance.class_ref &&
            a->instance.class_ref == b->instance.class_ref) {
            // Same class, check type arguments
            if (a->instance.type_arg_count != b->instance.type_arg_count)
                return false;
            for (int i = 0; i < a->instance.type_arg_count; i++) {
                if (!xr_type_equals(a->instance.type_args[i], b->instance.type_args[i])) {
                    return false;
                }
            }
            return true;
        }

        if (!a->instance.class_name || !b->instance.class_name)
            return false;

        // Compare base class names
        if (strcmp(a->instance.class_name, b->instance.class_name) != 0)
            return false;

        // Compare type arguments
        if (a->instance.type_arg_count != b->instance.type_arg_count)
            return false;
        for (int i = 0; i < a->instance.type_arg_count; i++) {
            if (!xr_type_equals(a->instance.type_args[i], b->instance.type_args[i])) {
                return false;
            }
        }
        return true;
    }
    if (a->kind == XR_KIND_FUNCTION) {
        if (a->function.param_count != b->function.param_count)
            return false;
        if (a->function.min_params != b->function.min_params)
            return false;
        if (a->function.is_variadic != b->function.is_variadic)
            return false;
        if (a->function.is_c_abi != b->function.is_c_abi)
            return false;
        if (a->function.receiver_mode != b->function.receiver_mode)
            return false;
        if (a->function.throw_effect != b->function.throw_effect)
            return false;
        if (a->function.view_return_source != b->function.view_return_source ||
            a->function.view_return_param != b->function.view_return_param ||
            a->function.view_return_complete != b->function.view_return_complete)
            return false;
        if (!function_type_params_equal(a, b))
            return false;
        if (!xr_type_equals(a->function.return_type, b->function.return_type))
            return false;
        for (int i = 0; i < a->function.param_count; i++) {
            if (xr_type_function_param_mode(a, i) != xr_type_function_param_mode(b, i))
                return false;
            if (!xr_type_equals(xr_type_function_param_type(a, i),
                                xr_type_function_param_type(b, i))) {
                return false;
            }
        }
        return true;
    }
    if (XR_TYPE_HAS_OBJECT_SHAPE(a)) {
        if (a->object.field_count != b->object.field_count)
            return false;
        for (int i = 0; i < a->object.field_count; i++) {
            if (!a->object.field_names || !b->object.field_names)
                continue;
            if (!a->object.field_names[i] || !b->object.field_names[i])
                continue;
            if (strcmp(a->object.field_names[i], b->object.field_names[i]) != 0) {
                return false;
            }
            // Compare field types (not just names)
            if (a->object.field_types && b->object.field_types && a->object.field_types[i] &&
                b->object.field_types[i]) {
                if (!xr_type_equals(a->object.field_types[i], b->object.field_types[i])) {
                    return false;
                }
            }
            bool ar = a->object.field_readonly ? a->object.field_readonly[i] : false;
            bool br = b->object.field_readonly ? b->object.field_readonly[i] : false;
            if (ar != br)
                return false;
        }
        if (a->object.type_name && b->object.type_name) {
            return strcmp(a->object.type_name, b->object.type_name) == 0;
        }
        return a->object.type_name == b->object.type_name;
    }
    if (a->kind == XR_KIND_TYPE_PARAM) {
        return a->type_param.id == b->type_param.id;
    }
    if (a->kind == XR_KIND_UNION) {
        if (a->union_type.member_count != b->union_type.member_count)
            return false;
        // Both sorted by kind, element-wise compare
        for (int i = 0; i < a->union_type.member_count; i++) {
            if (!xr_type_equals(a->union_type.members[i], b->union_type.members[i]))
                return false;
        }
        return true;
    }
    if (a->kind == XR_KIND_FIXED_ARRAY) {
        return a->fixed_array.length == b->fixed_array.length &&
               xr_type_equals(a->fixed_array.element_type, b->fixed_array.element_type);
    }
    if (a->kind == XR_KIND_TUPLE) {
        /* Structural equivalence: same arity and element-wise equal
         * types. Unit (XR_KIND_UNIT) is a distinct kind, already
         * accepted by the early `a->kind != b->kind` guard, so the
         * empty tuple shape never reaches this branch. */
        if (a->tuple.element_count != b->tuple.element_count)
            return false;
        for (int i = 0; i < a->tuple.element_count; i++) {
            if (!xr_type_equals(a->tuple.element_types[i], b->tuple.element_types[i]))
                return false;
        }
        return true;
    }

    return true;
}

// Type narrowing: keep type only if it matches given kind
XrType *xr_type_filter(XrVMRuntime *X, XrType *type, XrTypeKind kind) {
    if (!type)
        return NULL;
    if (XR_TYPE_IS_UNION(type))
        return xr_type_union_filter(type, kind);
    if (type->kind == kind)
        return type;
    return xr_type_new_never(X);
}

// Type narrowing: exclude type if it matches given kind
XrType *xr_type_exclude(XrVMRuntime *X, XrType *type, XrTypeKind kind) {
    if (!type)
        return NULL;
    if (XR_TYPE_IS_UNION(type))
        return xr_type_union_remove(X, type, kind);
    if (type->kind == kind)
        return xr_type_new_never(X);
    return type;
}

// Remove null from type
XrType *xr_type_non_nullable(XrVMRuntime *X, XrType *type) {
    if (!type)
        return NULL;

    // If type is nullable, return non-nullable version
    if (type->is_nullable) {
        switch (type->kind) {
            case XR_KIND_INT:
                return xr_type_new_int_width(X, type->scalar_rep);
            case XR_KIND_FLOAT:
                return xr_type_new_float_width(X, type->scalar_rep);
            case XR_KIND_STRING:
                return xr_type_new_string(X);
            case XR_KIND_BOOL:
                return xr_type_new_bool(X);
            case XR_KIND_JSON:
                if (type->object.field_count == 0)
                    return &g_type_json;
                break;
            default:
                break;
        }
        XrType *result = xr_type_copy(X, type);
        if (result)
            result->is_nullable = false;
        return result;
    }

    if (type->kind == XR_KIND_NULL) {
        return xr_type_new_never(X);
    }

    return type;
}

// Moved to xtype_format.c: xr_type_to_string, xr_type_is_inherently_immutable,
//   xr_type_is_const, xr_type_make_const, xr_type_object_get_field
// Moved to xtype_generic.c: xr_type_substitute, xr_type_satisfies_constraint,
//   xr_type_is_iterable, xr_type_is_iterator

// Helper: compare base class names (strip generic parameters)
static bool class_names_match(const char *name_a, const char *name_b) {
    if (!name_a || !name_b)
        return false;
    const char *lt_a = strchr(name_a, '<');
    const char *lt_b = strchr(name_b, '<');
    size_t len_a = lt_a ? (size_t) (lt_a - name_a) : strlen(name_a);
    size_t len_b = lt_b ? (size_t) (lt_b - name_b) : strlen(name_b);
    return len_a == len_b && strncmp(name_a, name_b, len_a) == 0;
}

// Class inheritance: walk up superclass chain
// Walks two equivalent chains in tandem because not every code path linking
// XrClassInfo also re-creates the corresponding XrType: the analyzer links
// class_ref->base in Pass 1.5, while xr_type_new_instance() leaves
// superclass NULL. Either chain alone is sufficient when populated; we
// follow whichever has more reach.
bool xr_type_is_subclass_of(XrType *type, XrType *target) {
    if (!type || !target)
        return false;
    if (type->kind != XR_KIND_CLASS && type->kind != XR_KIND_INSTANCE)
        return false;
    if (target->kind != XR_KIND_CLASS && target->kind != XR_KIND_INSTANCE)
        return false;

    const char *target_name = target->instance.class_name;
    if (!target_name)
        return false;

    // Walk up inheritance chain via XrType.superclass first
    XrType *current = type;
    while (current) {
        if (current->instance.class_name &&
            class_names_match(current->instance.class_name, target_name)) {
            return true;
        }
        current = current->instance.superclass;
    }

    // Fallback: walk via XrClassInfo.base chain (set by analyzer Pass 1.5).
    // This is needed because xr_type_new_instance() doesn't propagate
    // superclass from the class declaration's XrType.
    XrClassInfo *info = type->instance.class_ref;
    while (info) {
        if (info->name && class_names_match(info->name, target_name)) {
            return true;
        }
        // Prelude built-in classes (Exception, Range, ...) have no
        // user-side XrClassInfo to link in, so info->base stays NULL
        // even when `extends Exception` is declared. Check the deferred
        // base_name string before walking up: if it matches the target
        // name, the user's `extends X` clause put X in the chain even
        // though no class_info exists for the built-in.
        if (info->base_name && class_names_match(info->base_name, target_name)) {
            return true;
        }
        info = info->base;
    }
    return false;
}
