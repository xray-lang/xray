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
#include "xtype_names.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "xtype_internal.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"

// ========== Process-level static singletons (early init) ==========
// Basic types are immutable and globally shared. No allocation needed.
static _Atomic bool g_types_initialized = false;

// Non-nullable singletons
static XrType g_type_int;
static XrType g_type_float;
static XrType g_type_string;
static XrType g_type_bool;
static XrType g_type_null;
static XrType g_type_unknown;
static XrType g_type_never;
static XrType g_type_void;
static XrType g_type_json;

// Nullable singletons (T?)
static XrType g_type_int_nullable;
static XrType g_type_float_nullable;
static XrType g_type_string_nullable;
static XrType g_type_bool_nullable;
static XrType g_type_json_nullable;

static void init_singleton(XrType *t, XrTypeKind kind, uint32_t id, bool nullable,
                           uint8_t native_width) {
    memset(t, 0, sizeof(XrType));
    t->kind = kind;
    t->id = id;
    t->frozen = true;
    t->is_nullable = nullable;
    t->native_width = native_width;
}

void xr_type_global_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_types_initialized, &expected, true))
        return;

    uint32_t id = 1;
    init_singleton(&g_type_int, XR_KIND_INT, id++, false, 0);
    init_singleton(&g_type_float, XR_KIND_FLOAT, id++, false, 0);
    init_singleton(&g_type_string, XR_KIND_STRING, id++, false, 0);
    init_singleton(&g_type_bool, XR_KIND_BOOL, id++, false, 0);
    init_singleton(&g_type_null, XR_KIND_NULL, id++, false, 0);
    init_singleton(&g_type_unknown, XR_KIND_UNKNOWN, id++, false, 0);
    init_singleton(&g_type_never, XR_KIND_NEVER, id++, false, 0);
    init_singleton(&g_type_void, XR_KIND_VOID, id++, false, 0);
    init_singleton(&g_type_json, XR_KIND_JSON, id++, false, 0);
    g_type_json.object.allow_extension = true;

    init_singleton(&g_type_int_nullable, XR_KIND_INT, id++, true, 0);
    init_singleton(&g_type_float_nullable, XR_KIND_FLOAT, id++, true, 0);
    init_singleton(&g_type_string_nullable, XR_KIND_STRING, id++, true, 0);
    init_singleton(&g_type_bool_nullable, XR_KIND_BOOL, id++, true, 0);
    init_singleton(&g_type_json_nullable, XR_KIND_JSON, id++, true, 0);
    g_type_json_nullable.object.allow_extension = true;
}

// Set current type pool on the active isolate (called by XaAnalyzer)
void xr_type_set_current_pool(XrTypePool *pool, uint32_t *id_counter) {
    (void) id_counter;  // ID counter now managed by pool itself
    XrayIsolate *X = xray_isolate_current();
    if (X)
        X->current_type_pool = pool;
}

// Get current type pool from active isolate (for rare no-X contexts like xr_type_to_string)
XrTypePool *xr_type_get_current_pool(void) {
    XrayIsolate *X = xray_isolate_current();
    return X ? X->current_type_pool : NULL;
}

// Resolve isolate: use explicit X, or fallback to TLS current isolate.
// Many callers (xr_type_to_string, xr_type_assignable, etc.) pass NULL.
static inline XrayIsolate *resolve_isolate(XrayIsolate *X) {
    return X ? X : xray_isolate_current();
}

// Helper: allocate and initialize a type (for non-singleton types)
// Uses pool arena for allocation - memory freed when pool is reset/destroyed
static XrType *type_alloc(XrayIsolate *X, XrTypeKind kind) {
    X = resolve_isolate(X);
    XR_CHECK(X != NULL, "type_alloc: no isolate (explicit or TLS)");
    XrTypePool *pool = X->current_type_pool;
    XR_CHECK(pool != NULL, "Type pool not set - ensure isolate has current_type_pool");
    return xr_pool_alloc_type(pool, kind);
}

XrType *xr_type_new(XrayIsolate *X, XrTypeKind kind) {
    return type_alloc(X, kind);
}

// Primitive type constructors (return process-level singletons)
XrType *xr_type_new_int(XrayIsolate *X) {
    (void) X;
    return &g_type_int;
}
XrType *xr_type_new_float(XrayIsolate *X) {
    (void) X;
    return &g_type_float;
}
XrType *xr_type_new_string(XrayIsolate *X) {
    (void) X;
    return &g_type_string;
}
XrType *xr_type_new_bool(XrayIsolate *X) {
    (void) X;
    return &g_type_bool;
}
XrType *xr_type_new_null(XrayIsolate *X) {
    (void) X;
    return &g_type_null;
}
XrType *xr_type_new_unknown(XrayIsolate *X) {
    (void) X;
    return &g_type_unknown;
}
XrType *xr_type_new_never(XrayIsolate *X) {
    (void) X;
    return &g_type_never;
}
XrType *xr_type_new_void(XrayIsolate *X) {
    (void) X;
    return &g_type_void;
}

// Native-width integer type (int8/16/32/64, uint8/16/32/64)
XrType *xr_type_new_int_width(XrayIsolate *X, int width) {
    XrType *type = type_alloc(X, XR_KIND_INT);
    if (!type)
        return NULL;
    type->native_width = (uint8_t) width;
    return type;
}

// Native-width float type (float32/64)
XrType *xr_type_new_float_width(XrayIsolate *X, int width) {
    XrType *type = type_alloc(X, XR_KIND_FLOAT);
    if (!type)
        return NULL;
    type->native_width = (uint8_t) width;
    return type;
}

// Container type constructors
XrType *xr_type_new_array(XrayIsolate *X, XrType *element_type) {
    XR_DCHECK(element_type != NULL, "type_new_array: NULL element_type");
    XrType *type = type_alloc(X, XR_KIND_ARRAY);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    return type;
}

XrType *xr_type_new_map(XrayIsolate *X, XrType *key_type, XrType *value_type) {
    XR_DCHECK(key_type != NULL, "type_new_map: NULL key_type");
    XR_DCHECK(value_type != NULL, "type_new_map: NULL value_type");
    XrType *type = type_alloc(X, XR_KIND_MAP);
    if (!type)
        return NULL;
    type->map.key_type = key_type;
    type->map.value_type = value_type;
    return type;
}

XrType *xr_type_new_set(XrayIsolate *X, XrType *element_type) {
    XrType *type = type_alloc(X, XR_KIND_SET);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    return type;
}

XrType *xr_type_new_channel(XrayIsolate *X, XrType *element_type) {
    XrType *type = type_alloc(X, XR_KIND_CHANNEL);
    if (!type)
        return NULL;
    type->container.element_type = element_type;
    return type;
}

XrType *xr_type_new_task(XrayIsolate *X, XrType *result_type) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (!type)
        return NULL;
    type->instance.class_name = "Task";
    if (result_type) {
        XrTypePool *pool = X->current_type_pool;
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
XrType *xr_type_new_json(XrayIsolate *X) {
    return &g_type_json;  // Process-level singleton (plain Json without fields)
}

XrType *xr_type_new_json_with_fields(XrayIsolate *X, const char **names, XrType **types,
                                     int count) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_JSON);
    if (!type)
        return NULL;
    XrTypePool *pool = X->current_type_pool;
    type->object.allow_extension = true;  // Json allows dynamic fields
    if (count > 0 && names && types) {
        type->object.field_count = count;
        type->object.field_names = (const char **) xr_pool_alloc(pool, sizeof(char *) * count);
        type->object.field_types = (XrType **) xr_pool_alloc(pool, sizeof(XrType *) * count);
        for (int i = 0; i < count; i++) {
            type->object.field_names[i] = names[i] ? xr_pool_strdup(pool, names[i]) : NULL;
            type->object.field_types[i] = types[i];
        }
    }
    return type;
}

// Structured object types (for JSON with known fields)
XrType *xr_type_new_object(XrayIsolate *X, const char **field_names, XrType **field_types,
                           int field_count, bool allow_extension, const char *type_name) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_JSON);
    if (!type)
        return NULL;
    XrTypePool *pool = X->current_type_pool;

    if (field_count > 0 && field_names) {
        type->object.field_names = xr_pool_alloc(pool, sizeof(const char *) * field_count);
        if (type->object.field_names) {
            for (int i = 0; i < field_count; i++) {
                type->object.field_names[i] =
                    field_names[i] ? xr_pool_strdup(pool, field_names[i]) : NULL;
            }
        }
    }

    if (field_count > 0 && field_types) {
        type->object.field_types = xr_pool_alloc(pool, sizeof(XrType *) * field_count);
        if (type->object.field_types) {
            memcpy(type->object.field_types, field_types, sizeof(XrType *) * field_count);
        }
    }

    type->object.field_count = field_count;
    type->object.allow_extension = allow_extension;
    type->object.type_name = type_name ? xr_pool_strdup(pool, type_name) : NULL;
    type->object.field_readonly = NULL;

    return type;
}

XrType *xr_type_new_object_anonymous(XrayIsolate *X, const char **field_names, XrType **field_types,
                                     int field_count) {
    return xr_type_new_object(X, field_names, field_types, field_count, true, NULL);
}

// Optional type (T?) - unified: uses is_nullable on the base type itself
XrType *xr_type_new_optional(XrayIsolate *X, XrType *base_type) {
    return xr_type_make_nullable(X, base_type);
}

XrType *xr_type_get_base(XrType *optional_type) {
    return xr_type_non_nullable(NULL, optional_type);
}

// Type parameter (for generics)
XrType *xr_type_new_type_param(XrayIsolate *X, const char *name, int id) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_TYPE_PARAM);
    if (!type)
        return NULL;
    XrTypePool *pool = X->current_type_pool;
    type->type_param.name = name ? xr_pool_strdup(pool, name) : NULL;
    type->type_param.id = id;
    type->type_param.constraint = NULL;
    return type;
}

XrType *xr_type_new_type_param_constrained(XrayIsolate *X, const char *name, int id,
                                           XrType *constraint) {
    XrType *type = xr_type_new_type_param(X, name, id);
    if (type) {
        type->type_param.constraint = constraint;
    }
    return type;
}

XrType *xr_type_new_class(XrayIsolate *X, const char *class_name) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_CLASS);
    if (type && class_name) {
        XrTypePool *pool = X->current_type_pool;
        type->instance.class_name = xr_pool_strdup(pool, class_name);
    }
    return type;
}

XrType *xr_type_new_interface(XrayIsolate *X, const char *interface_name) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INTERFACE);
    if (type && interface_name) {
        XrTypePool *pool = X->current_type_pool;
        type->instance.class_name = xr_pool_strdup(pool, interface_name);
    }
    return type;
}

XrType *xr_type_new_bigint(XrayIsolate *X) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = "BigInt";
    return type;
}

XrType *xr_type_new_datetime(XrayIsolate *X) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = "DateTime";
    return type;
}

XrType *xr_type_new_bytes(XrayIsolate *X) {
    XrType *type = type_alloc(X, XR_KIND_BYTES);
    return type;
}

XrType *xr_type_new_regex(XrayIsolate *X) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = "Regex";
    return type;
}

XrType *xr_type_new_stringbuilder(XrayIsolate *X) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = "StringBuilder";
    return type;
}

XrType *xr_type_new_named_instance(XrayIsolate *X, const char *name) {
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (type)
        type->instance.class_name = name;
    return type;
}

XrType *xr_type_new_enum(XrayIsolate *X, const char *enum_name) {
    XrType *type = type_alloc(X, XR_KIND_ENUM);
    if (type && enum_name) {
        type->enum_type.enum_name = enum_name;
    }
    return type;
}

XrType *xr_type_new_instance(XrayIsolate *X, XrClassInfo *class_info) {
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (!type)
        return NULL;
    type->instance.class_ref = class_info;
    // Extract class_name from class_info for type comparison and display
    if (class_info && class_info->name) {
        XrTypePool *pool = X->current_type_pool;
        type->instance.class_name = xr_pool_strdup(pool, class_info->name);
    }
    type->instance.type_args = NULL;
    type->instance.type_arg_count = 0;
    return type;
}

XrType *xr_type_new_generic_instance(XrayIsolate *X, const char *class_name,
                                     XrClassInfo *class_info, XrType **type_args,
                                     int type_arg_count) {
    XR_DCHECK(type_arg_count >= 0, "type_new_generic_instance: negative type_arg_count");
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_INSTANCE);
    if (!type)
        return NULL;
    XrTypePool *pool = X->current_type_pool;

    type->instance.class_name = class_name ? xr_pool_strdup(pool, class_name) : NULL;
    type->instance.class_ref = class_info;
    type->instance.superclass = NULL;

    // Copy type arguments
    type->instance.type_arg_count = type_arg_count;
    if (type_arg_count > 0 && type_args) {
        type->instance.type_args = xr_pool_alloc(pool, sizeof(XrType *) * type_arg_count);
        if (type->instance.type_args) {
            for (int i = 0; i < type_arg_count; i++) {
                type->instance.type_args[i] = type_args[i];
            }
        }
    } else {
        type->instance.type_args = NULL;
    }

    return type;
}

// Function type
XrType *xr_type_new_function(XrayIsolate *X, XrType **param_types, int param_count,
                             XrType *return_type, bool is_variadic) {
    XR_DCHECK(param_count >= 0, "type_new_function: negative param_count");
    XR_DCHECK(param_count == 0 || param_types != NULL,
              "type_new_function: NULL param_types with count > 0");
    X = resolve_isolate(X);
    XrType *type = type_alloc(X, XR_KIND_FUNCTION);
    if (!type)
        return NULL;
    XrTypePool *pool = X->current_type_pool;

    if (param_count > 0 && param_types) {
        type->function.param_types = xr_pool_alloc(pool, sizeof(XrType *) * param_count);
        if (type->function.param_types) {
            memcpy(type->function.param_types, param_types, sizeof(XrType *) * param_count);
        }
    }
    type->function.param_count = param_count;
    type->function.min_params = param_count;  // Default: all params required
    type->function.return_type = return_type;
    type->function.is_variadic = is_variadic;
    return type;
}

// Tuple type (for multi-value return, compile-time only)
XrType *xr_type_new_tuple(XrayIsolate *X, XrType **element_types, int count) {
    X = resolve_isolate(X);
    if (count <= 0)
        return xr_type_new_void(X);
    if (count == 1 && element_types && element_types[0]) {
        return element_types[0];  // Single element is not a tuple
    }

    XrType *type = type_alloc(X, XR_KIND_TUPLE);
    if (!type)
        return NULL;
    XrTypePool *pool = X->current_type_pool;

    if (count > 0 && element_types) {
        type->tuple.element_types = xr_pool_alloc(pool, sizeof(XrType *) * count);
        if (type->tuple.element_types) {
            memcpy(type->tuple.element_types, element_types, sizeof(XrType *) * count);
        }
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
    bool has_int = false;
    bool has_float = false;
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
        if (XR_TYPE_IS_INT(t))
            has_int = true;
        if (XR_TYPE_IS_FLOAT(t))
            has_float = true;

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

    // Numeric promotion: int | float -> keep float only
    if (has_int && has_float) {
        for (int i = 0; i < out_count; i++) {
            if (XR_TYPE_IS_INT(out[i])) {
                out[i] = out[out_count - 1];
                out_count--;
                i--;
            }
        }
    }

    *out_nullable = has_null;
    return out_count;
}

// Construct a union type from an array of member types.
// Applies: flatten existing unions, dedup, sort, special rules.
// Union aliases as members are flattened (from xr_type_union merge path).
XrType *xr_type_new_union(XrayIsolate *X, XrType **members, int count) {
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
    XrTypePool *pool = X->current_type_pool;
    type->union_type.members = (XrType **) xr_pool_alloc(pool, sizeof(XrType *) * result_count);
    if (!type->union_type.members)
        return xr_type_new_unknown(X);
    memcpy(type->union_type.members, result, sizeof(XrType *) * result_count);
    type->union_type.member_count = (uint8_t) result_count;
    if (has_null)
        type->is_nullable = true;
    return type;
}

// Merge two types into a union (or apply special rules).
// Type union rules: int|float->float, T|null->T?
XrType *xr_type_union(XrayIsolate *X, XrType *a, XrType *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (xr_type_equals(a, b))
        return a;

    // unknown | T = unknown
    if (XR_TYPE_IS_UNKNOWN(a) || XR_TYPE_IS_UNKNOWN(b))
        return xr_type_new_unknown(X);

    // never | T = T
    if (XR_TYPE_IS_NEVER(a))
        return b;
    if (XR_TYPE_IS_NEVER(b))
        return a;

    // Numeric promotion: int | float -> float
    if (XR_TYPE_IS_INT(a) && XR_TYPE_IS_FLOAT(b))
        return b;
    if (XR_TYPE_IS_FLOAT(a) && XR_TYPE_IS_INT(b))
        return a;

    // T | null = T? (nullable shortcut)
    if (XR_TYPE_IS_NULL(a) && !XR_TYPE_IS_NULL(b))
        return xr_type_make_nullable(X, b);
    if (XR_TYPE_IS_NULL(b) && !XR_TYPE_IS_NULL(a))
        return xr_type_make_nullable(X, a);

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

// Remove all members with given kind from union; return simplified type
XrType *xr_type_union_remove(XrayIsolate *X, XrType *type, XrTypeKind kind) {
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

// Fixed-length array ([N]T - compile-time length, runtime Array<T>)
XrType *xr_type_new_fixed_array(XrayIsolate *X, XrType *element_type, int length) {
    XrType *type = type_alloc(X, XR_KIND_FIXED_ARRAY);
    if (!type)
        return NULL;
    type->fixed_array.element_type = element_type;
    type->fixed_array.length = length;
    return type;
}

// Copy a type
XrType *xr_type_copy(XrayIsolate *X, XrType *type) {
    if (!type)
        return NULL;
    XR_DCHECK(type->kind < XR_KIND_COUNT, "type_copy: invalid kind");
    // Resolve X early for callers that pass NULL (e.g. xr_type_to_string)
    if (!X)
        X = xray_isolate_current();

    XrType *copy = type_alloc(X, type->kind);
    if (!copy)
        return NULL;

    XrTypePool *pool = X->current_type_pool;

    copy->is_nullable = type->is_nullable;

    copy->is_const = type->is_const;
    copy->native_width = type->native_width;

    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
            copy->container.element_type = type->container.element_type;
            break;
        case XR_KIND_MAP:
            copy->map.key_type = type->map.key_type;
            copy->map.value_type = type->map.value_type;
            break;
        case XR_KIND_ENUM:
            copy->enum_type.enum_name =
                type->enum_type.enum_name ? xr_pool_strdup(pool, type->enum_type.enum_name) : NULL;
            break;
        case XR_KIND_INSTANCE:
        case XR_KIND_CLASS:
            copy->instance.class_name =
                type->instance.class_name ? xr_pool_strdup(pool, type->instance.class_name) : NULL;
            copy->instance.class_ref = type->instance.class_ref;
            copy->instance.superclass = type->instance.superclass;
            copy->instance.type_arg_count = type->instance.type_arg_count;
            if (type->instance.type_arg_count > 0 && type->instance.type_args) {
                copy->instance.type_args =
                    xr_pool_alloc(pool, sizeof(XrType *) * type->instance.type_arg_count);
                if (copy->instance.type_args) {
                    memcpy(copy->instance.type_args, type->instance.type_args,
                           sizeof(XrType *) * type->instance.type_arg_count);
                }
            } else {
                copy->instance.type_args = NULL;
            }
            break;
        case XR_KIND_FUNCTION:
            if (type->function.param_count > 0) {
                copy->function.param_types =
                    xr_pool_alloc(pool, sizeof(XrType *) * type->function.param_count);
                if (copy->function.param_types) {
                    memcpy(copy->function.param_types, type->function.param_types,
                           sizeof(XrType *) * type->function.param_count);
                }
            }
            copy->function.param_count = type->function.param_count;
            copy->function.return_type = type->function.return_type;
            copy->function.is_variadic = type->function.is_variadic;
            break;
        case XR_KIND_JSON:
            if (type->object.field_count > 0) {
                copy->object.field_count = type->object.field_count;
                copy->object.allow_extension = type->object.allow_extension;
                copy->object.type_name =
                    type->object.type_name ? xr_pool_strdup(pool, type->object.type_name) : NULL;
                if (type->object.field_names) {
                    copy->object.field_names =
                        xr_pool_alloc(pool, sizeof(const char *) * type->object.field_count);
                    if (copy->object.field_names) {
                        for (int i = 0; i < type->object.field_count; i++) {
                            copy->object.field_names[i] =
                                type->object.field_names[i]
                                    ? xr_pool_strdup(pool, type->object.field_names[i])
                                    : NULL;
                        }
                    }
                }
                if (type->object.field_types) {
                    copy->object.field_types =
                        xr_pool_alloc(pool, sizeof(XrType *) * type->object.field_count);
                    if (copy->object.field_types) {
                        memcpy(copy->object.field_types, type->object.field_types,
                               sizeof(XrType *) * type->object.field_count);
                    }
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
                copy->union_type.members =
                    xr_pool_alloc(pool, sizeof(XrType *) * type->union_type.member_count);
                if (copy->union_type.members) {
                    memcpy(copy->union_type.members, type->union_type.members,
                           sizeof(XrType *) * type->union_type.member_count);
                }
            } else {
                copy->union_type.members = NULL;
            }
            break;
        default:
            break;
    }

    return copy;
}

// Make a type nullable (returns cached singleton for primitive types)
XrType *xr_type_make_nullable(XrayIsolate *X, XrType *type) {
    if (!type)
        return NULL;
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
    if (type == &g_type_json)
        return &g_type_json_nullable;

    // Also handle pool singletons (frozen types from type pool)
    if (type->frozen && X && X->current_type_pool) {
        switch (type->kind) {
            case XR_KIND_INT:
                return &g_type_int_nullable;
            case XR_KIND_FLOAT:
                return &g_type_float_nullable;
            case XR_KIND_STRING:
                return &g_type_string_nullable;
            case XR_KIND_BOOL:
                return &g_type_bool_nullable;
            case XR_KIND_JSON:
                if (type->object.field_count == 0)
                    return &g_type_json_nullable;
                break;
            default:
                break;
        }
    }

    // Frozen singleton without cached nullable: must copy before mutation
    if (type->frozen) {
        XrType *copy = xr_type_copy(X, type);
        if (copy)
            copy->is_nullable = true;
        return copy;
    }

    type->is_nullable = true;
    return type;
}

// Check if source type is assignable to target type
// This is the core type compatibility check, migrated from sema/ct_compatible
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

    // null is compatible with nullable type (T?)
    if (XR_TYPE_IS_NULL(source) && target->is_nullable)
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
    if (source->is_nullable && !target->is_nullable) {
        return false;
    }

    // Numeric promotion: int -> float
    if (XR_TYPE_IS_FLOAT(target) && XR_TYPE_IS_INT(source))
        return true;

    // Json compatibility - Map, string, int, float, bool → Json (Array is NOT Json)
    if (XR_TYPE_IS_JSON(target) && !XR_TYPE_IS_JSON(source)) {
        if (XR_TYPE_IS_MAP(source) || XR_TYPE_IS_STRING(source) || XR_TYPE_IS_INT(source) ||
            XR_TYPE_IS_FLOAT(source) || XR_TYPE_IS_BOOL(source)) {
            return true;
        }
    }

    // JSON object structural subtyping
    if (XR_TYPE_IS_JSON(target) && XR_TYPE_IS_JSON(source)) {
        // If target has no fields (plain Json), accept any object
        if (target->object.field_count == 0)
            return true;

        // If source has no fields (plain Json), accept assignment to structured
        // Json. Json is the explicit dynamic data boundary; codegen inserts
        // runtime validation for downstream coercions.
        if (source->object.field_count == 0)
            return true;

        // Check structural compatibility (source has all target fields)
        for (int i = 0; i < target->object.field_count; i++) {
            XrType *target_field_type =
                target->object.field_types ? target->object.field_types[i] : NULL;

            // Check if field is optional
            bool is_optional = target_field_type && target_field_type->is_nullable;

            bool found = false;
            for (int j = 0; j < source->object.field_count; j++) {
                if (target->object.field_names && source->object.field_names &&
                    target->object.field_names[i] && source->object.field_names[j] &&
                    strcmp(target->object.field_names[i], source->object.field_names[j]) == 0) {
                    // Check field type compatibility
                    if (target_field_type && source->object.field_types) {
                        XrType *source_field_type = source->object.field_types[j];
                        XrType *check_type = is_optional
                                                 ? xr_type_non_nullable(NULL, target_field_type)
                                                 : target_field_type;
                        if (!xr_type_assignable(check_type, source_field_type)) {
                            return false;
                        }
                    }
                    found = true;
                    break;
                }
            }
            // Optional fields can be missing
            if (!found && !is_optional && !target->object.allow_extension) {
                return false;
            }
        }
        return true;
    }

    // Bytes ↔ Array: Bytes is an alias for Array<uint8>
    if (target->kind == XR_KIND_BYTES && XR_TYPE_IS_ARRAY(source))
        return true;
    if (XR_TYPE_IS_ARRAY(target) && source->kind == XR_KIND_BYTES)
        return true;
    if (target->kind == XR_KIND_BYTES && source->kind == XR_KIND_BYTES)
        return true;

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

    // Fixed-length array compatibility: [N]T <-> Array<T>
    if (target->kind == XR_KIND_FIXED_ARRAY && XR_TYPE_IS_ARRAY(source)) {
        if (!target->fixed_array.element_type || !source->container.element_type)
            return true;
        return xr_type_assignable(target->fixed_array.element_type, source->container.element_type);
    }
    if (XR_TYPE_IS_ARRAY(target) && source->kind == XR_KIND_FIXED_ARRAY) {
        if (!target->container.element_type || !source->fixed_array.element_type)
            return true;
        return xr_type_assignable(target->container.element_type, source->fixed_array.element_type);
    }
    if (target->kind == XR_KIND_FIXED_ARRAY && source->kind == XR_KIND_FIXED_ARRAY) {
        if (target->fixed_array.length != source->fixed_array.length)
            return false;
        if (!target->fixed_array.element_type || !source->fixed_array.element_type)
            return true;
        return xr_type_equals(target->fixed_array.element_type, source->fixed_array.element_type);
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

    // Task type compatibility: both are INSTANCE with class_name="Task"
    if (xr_type_is_named_class(target, "Task") && xr_type_is_named_class(source, "Task")) {
        XrType *tr = (target->instance.type_arg_count > 0) ? target->instance.type_args[0] : NULL;
        XrType *sr = (source->instance.type_arg_count > 0) ? source->instance.type_args[0] : NULL;
        if (!tr || !sr)
            return true;
        return xr_type_assignable(tr, sr);
    }

    // Function type compatibility
    if (XR_TYPE_IS_FUNCTION(target) && XR_TYPE_IS_FUNCTION(source)) {
        // Parameter count must match (or target is variadic)
        if (target->function.param_count != source->function.param_count) {
            return false;
        }

        // Return type: covariant - source return must be assignable to target return
        // Special case: void target accepts any return type
        if (target->function.return_type && source->function.return_type) {
            if (!XR_TYPE_IS_VOID(target->function.return_type)) {
                if (!xr_type_assignable(target->function.return_type,
                                        source->function.return_type)) {
                    return false;
                }
            }
        }

        // Parameters: contravariant - target params assignable to source params
        // Simplified: allow if types match or either side is unknown/type_param
        for (int i = 0; i < target->function.param_count; i++) {
            XrType *t_param = target->function.param_types ? target->function.param_types[i] : NULL;
            XrType *s_param = source->function.param_types ? source->function.param_types[i] : NULL;

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
        // interface resolved by parser as class name).
        const char *target_name = target->instance.class_name;
        if (target_name) {
            for (XrClassInfo *ci = source->instance.class_ref; ci; ci = ci->base) {
                for (int i = 0; i < ci->interface_count; i++) {
                    if (ci->interface_names[i] && strcmp(ci->interface_names[i], target_name) == 0)
                        return true;
                }
            }
        }
        return false;
    }

    return false;
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

    // Check type-specific data
    if (a->kind == XR_KIND_ARRAY || a->kind == XR_KIND_SET || a->kind == XR_KIND_CHANNEL) {
        return xr_type_equals(a->container.element_type, b->container.element_type);
    }
    if (a->kind == XR_KIND_MAP) {
        return xr_type_equals(a->map.key_type, b->map.key_type) &&
               xr_type_equals(a->map.value_type, b->map.value_type);
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
        if (!xr_type_equals(a->function.return_type, b->function.return_type))
            return false;
        for (int i = 0; i < a->function.param_count; i++) {
            if (!xr_type_equals(a->function.param_types[i], b->function.param_types[i])) {
                return false;
            }
        }
        return true;
    }
    if (a->kind == XR_KIND_JSON) {
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

    return true;
}

// Type narrowing: keep type only if it matches given kind
XrType *xr_type_filter(XrayIsolate *X, XrType *type, XrTypeKind kind) {
    if (!type)
        return NULL;
    if (XR_TYPE_IS_UNION(type))
        return xr_type_union_filter(type, kind);
    if (type->kind == kind)
        return type;
    return xr_type_new_never(X);
}

// Type narrowing: exclude type if it matches given kind
XrType *xr_type_exclude(XrayIsolate *X, XrType *type, XrTypeKind kind) {
    if (!type)
        return NULL;
    if (XR_TYPE_IS_UNION(type))
        return xr_type_union_remove(X, type, kind);
    if (type->kind == kind)
        return xr_type_new_never(X);
    return type;
}

// Remove null from type
XrType *xr_type_non_nullable(XrayIsolate *X, XrType *type) {
    if (!type)
        return NULL;

    // If type is nullable, return non-nullable version
    if (type->is_nullable) {
        switch (type->kind) {
            case XR_KIND_INT:
                return xr_type_new_int(X);
            case XR_KIND_FLOAT:
                return xr_type_new_float(X);
            case XR_KIND_STRING:
                return xr_type_new_string(X);
            case XR_KIND_BOOL:
                return xr_type_new_bool(X);
            case XR_KIND_JSON:
                if (type == &g_type_json_nullable ||
                    (type->object.field_count == 0 && type->object.allow_extension))
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

    // Walk up inheritance chain
    XrType *current = type;
    while (current) {
        if (current->instance.class_name &&
            class_names_match(current->instance.class_name, target_name)) {
            return true;
        }
        current = current->instance.superclass;
    }
    return false;
}
