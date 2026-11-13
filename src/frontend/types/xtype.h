/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype.h - Type construction, conversion and query API
 *
 * KEY CONCEPT:
 *   Each XrType has exactly one XrTypeKind.
 *   Supports union types (int | string) and T? (nullable) via is_nullable flag.
 *   Category checks (numeric, primitive, etc.) use inline functions.
 */

#ifndef XTYPE_H
#define XTYPE_H

#include "xtype_repr.h"

// Type checking macros
#define XR_TYPE_IS_INT(t) ((t)->kind == XR_KIND_INT)
#define XR_TYPE_IS_FLOAT(t) ((t)->kind == XR_KIND_FLOAT)
#define XR_TYPE_IS_STRING(t) ((t)->kind == XR_KIND_STRING)
#define XR_TYPE_IS_BOOL(t) ((t)->kind == XR_KIND_BOOL)
#define XR_TYPE_IS_RUNE(t) ((t)->kind == XR_KIND_RUNE)
#define XR_TYPE_IS_NULL(t) ((t)->kind == XR_KIND_NULL)
#define XR_TYPE_IS_NUMERIC(t) (xr_kind_is_numeric((t)->kind))
#define XR_TYPE_IS_PRIMITIVE(t) (xr_kind_is_primitive((t)->kind))
#define XR_TYPE_IS_ARRAY(t) ((t)->kind == XR_KIND_ARRAY)
#define XR_TYPE_IS_SLICE(t) ((t)->kind == XR_KIND_SLICE)
#define XR_TYPE_IS_MAP(t) ((t)->kind == XR_KIND_MAP)
#define XR_TYPE_IS_SET(t) ((t)->kind == XR_KIND_SET)
#define XR_TYPE_IS_FUNCTION(t) ((t)->kind == XR_KIND_FUNCTION)
#define XR_TYPE_IS_C_FUNCTION(t) ((t)->kind == XR_KIND_FUNCTION && (t)->function.is_c_abi)
#define XR_TYPE_IS_INSTANCE(t) ((t)->kind == XR_KIND_INSTANCE)
#define XR_TYPE_IS_UNKNOWN(t) ((t)->kind == XR_KIND_UNKNOWN)
#define XR_TYPE_IS_ERROR(t) ((t)->kind == XR_KIND_ERROR)
#define XR_TYPE_IS_UNKNOWN_OR_ERROR(t) ((t)->kind == XR_KIND_UNKNOWN || (t)->kind == XR_KIND_ERROR)
#define XR_TYPE_IS_NEVER(t) ((t)->kind == XR_KIND_NEVER)
#define XR_TYPE_IS_CLASS(t) ((t)->kind == XR_KIND_CLASS)
#define XR_TYPE_IS_INTERFACE(t) ((t)->kind == XR_KIND_INTERFACE)
#define XR_TYPE_IS_NULLABLE(t) ((t)->is_nullable || ((t)->kind == XR_KIND_NULL))
#define XR_TYPE_IS_JSON(t) ((t)->kind == XR_KIND_JSON)
#define XR_TYPE_IS_STRUCT_OBJECT(t) ((t)->kind == XR_KIND_STRUCT_OBJECT)
#define XR_TYPE_HAS_OBJECT_SHAPE(t) ((t) && xr_kind_has_object_shape((t)->kind))
#define XR_TYPE_IS_TYPE_PARAM(t) ((t)->kind == XR_KIND_TYPE_PARAM)
#define XR_TYPE_IS_TUPLE(t) ((t)->kind == XR_KIND_TUPLE)

/* JSON.Object is a pure alias, not a nominal wrapper. Recognizing the exact
 * Map instantiation here gives diagnostics and formatting one canonical public
 * spelling while type identity remains Map<string, JSON.Value>. */
static inline bool xr_type_is_json_object(const XrType *t) {
    return t && t->kind == XR_KIND_MAP && t->map.key_type && t->map.value_type &&
           t->map.key_type->kind == XR_KIND_STRING && !t->map.key_type->is_nullable &&
           t->map.value_type->kind == XR_KIND_JSON && !t->map.value_type->is_nullable;
}
// Unit type is the canonical "no meaningful value" type for functions
// returning nothing. Spelled `()` in user syntax (0-arity tuple literal),
// stored internally as a dedicated XR_KIND_UNIT kind for fast dispatch.
#define XR_TYPE_IS_UNIT(t) ((t)->kind == XR_KIND_UNIT)
#define XR_TYPE_IS_OPTIONAL(t) ((t)->is_nullable)
#define XR_TYPE_IS_ENUM(t) ((t)->kind == XR_KIND_ENUM)
#define XR_TYPE_IS_UNION(t) ((t)->kind == XR_KIND_UNION)
#define XR_TYPE_IS_POINTER(t) ((t)->kind == XR_KIND_POINTER)

XR_FUNC bool xr_type_contains_error(const XrType *type);

static inline bool xr_type_is_exact_u8(const XrType *type) {
    return type && type->kind == XR_KIND_INT && type->scalar_rep == XR_NATIVE_U8;
}

static inline bool xr_type_is_exact_unsigned_integer(const XrType *type) {
    if (!type || type->kind != XR_KIND_INT || type->is_nullable)
        return false;
    switch (type->scalar_rep) {
        case XR_NATIVE_U8:
        case XR_NATIVE_U16:
        case XR_NATIVE_U32:
        case XR_NATIVE_U64:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static inline const XrType *xr_type_contiguous_element_type(const XrType *type) {
    if (!type)
        return NULL;
    if (type->kind == XR_KIND_ARRAY || type->kind == XR_KIND_SLICE)
        return type->container.element_type;
    if (type->kind == XR_KIND_FIXED_ARRAY)
        return type->fixed_array.element_type;
    return NULL;
}

static inline bool xr_type_is_u8_array(const XrType *type) {
    return type && type->kind == XR_KIND_ARRAY && xr_type_is_exact_u8(type->container.element_type);
}

static inline bool xr_type_is_u8_slice(const XrType *type) {
    return type && type->kind == XR_KIND_SLICE && xr_type_is_exact_u8(type->container.element_type);
}

static inline bool xr_type_is_u8_pointer(const XrType *type) {
    return type && type->kind == XR_KIND_POINTER &&
           xr_type_is_exact_u8(type->container.element_type);
}

static inline bool xr_type_is_u8_contiguous(const XrType *type) {
    return xr_type_is_exact_u8(xr_type_contiguous_element_type(type));
}


// Derive base rep ignoring nullable flag (for nullable optimization)
static inline XrRep xr_type_base_rep(const XrType *t) {
    if (!t)
        return XR_REP_TAGGED;
    if (xr_type_is_enum_metadata(t))
        return XR_REP_I64;
    switch (t->kind) {
        case XR_KIND_INT:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
        case XR_KIND_POINTER:
            /* Raw pointer = address-width integer; GC never scans it. */
            return XR_REP_I64;
        case XR_KIND_FLOAT:
            return XR_REP_F64;
        case XR_KIND_UNIT:
            return XR_REP_VOID;
        /* char keeps the tagged representation so the XR_TAG_RUNE identity
         * survives across slots, print and typeof. A raw u32 rep is a future
         * AOT optimization, not needed for correctness. */
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_TUPLE:
        case XR_KIND_JSON:
        case XR_KIND_STRUCT_OBJECT:
        case XR_KIND_INSTANCE:
        case XR_KIND_CHANNEL:
        case XR_KIND_INTERFACE:
        case XR_KIND_CLASS:
        case XR_KIND_FUNCTION:
        case XR_KIND_ENUM:
            return XR_REP_PTR;
        default:
            return XR_REP_TAGGED;
    }
}

// Derive machine representation from XrType
static inline XrRep xr_type_rep(const XrType *t) {
    if (!t)
        return XR_REP_TAGGED;
    if (t->is_nullable) {
        // PTR-based T?: null(0) vs non-null(ptr) distinguishable by payload.
        // I64/F64 nullable: int(0)/float(0.0) and null both have payload=0.
        XrRep base = xr_type_base_rep(t);
        return (base == XR_REP_PTR) ? XR_REP_PTR : XR_REP_TAGGED;
    }
    if (t->kind == XR_KIND_UNION) {
        /* Enum metadata descriptors are scalar ordinals only while their
         * concrete owner E remains statically known.  A union is an erased
         * identity boundary: even if all members happen to use I64, the
         * runtime value must retain owner/kind identity. */
        for (int i = 0; i < t->union_type.member_count; i++) {
            if (xr_type_is_enum_metadata(t->union_type.members[i]))
                return XR_REP_TAGGED;
        }
        // If all remaining members share the same rep, use it; otherwise TAGGED
        if (t->union_type.member_count == 0)
            return XR_REP_TAGGED;
        XrRep common = xr_type_rep(t->union_type.members[0]);
        for (int i = 1; i < t->union_type.member_count; i++) {
            if (xr_type_rep(t->union_type.members[i]) != common)
                return XR_REP_TAGGED;
        }
        return common;
    }
    return xr_type_base_rep(t);
}

// Type relation for comparison
typedef enum XrTypeRelation {
    XR_TYPE_EQUAL,         // Exactly the same type
    XR_TYPE_SUBTYPE,       // Can be assigned (source is subtype of target)
    XR_TYPE_SUPERTYPE,     // Target is subtype of source
    XR_TYPE_INCOMPATIBLE,  // Cannot be assigned
} XrTypeRelation;

// API: Type creation
XR_FUNC XrType *xr_type_new(XrVMRuntime *X, XrTypeKind kind);
XR_FUNC XrType *xr_type_new_int(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_float(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_string(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_bool(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_rune(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_null(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_unknown(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_error(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_never(XrVMRuntime *X);
// Unit type singleton (XR_KIND_UNIT, spelled `()` in user syntax). The
// canonical "no meaningful value" type used for functions that return
// nothing and for the empty tuple literal.
XR_FUNC XrType *xr_type_new_unit(XrVMRuntime *X);

// API: Exact scalar representations.
XR_FUNC XrType *xr_type_new_int_width(XrVMRuntime *X, int width);    // XrNativeType value
XR_FUNC XrType *xr_type_new_float_width(XrVMRuntime *X, int width);  // XrNativeType value

// API: FFI raw pointer type. element_type = pointee (T), is_mut selects
// MutPtr<T> (true) vs Ptr<T> (false, const). Address-width int at the value
// level; the GC never scans it.
XR_FUNC XrType *xr_type_new_pointer(XrVMRuntime *X, XrType *element_type, bool is_mut);

// API: Derive XrSlotType from XrType for the unified type pipeline.
// Returns the storage slot type — used by GC scanning and AOT codegen.
// scalar_rep stores XrNativeType; slot storage widens integer and float scalars.
static inline uint8_t xr_type_to_slot_type(XrType *type) {
    if (!type)
        return XR_SLOT_ANY;
    if (xr_type_is_enum_metadata(type))
        return XR_SLOT_I64;
    if (!type->is_nullable && (type->kind == XR_KIND_INT || type->kind == XR_KIND_FLOAT)) {
        uint8_t rep = type->scalar_rep;
        if (rep == XR_NATIVE_F32 || rep == XR_NATIVE_F64)
            return XR_SLOT_F64;
        return XR_SLOT_I64;
    }
    switch (type->kind) {
        case XR_KIND_INT:
            return XR_SLOT_I64;
        case XR_KIND_POINTER:
            /* Raw pointer = address-width integer slot (GC-invisible). */
            return XR_SLOT_I64;
        case XR_KIND_FLOAT:
            return XR_SLOT_F64;
        case XR_KIND_BOOL:
            return XR_SLOT_BOOL;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_TUPLE:
        case XR_KIND_JSON:
        case XR_KIND_STRUCT_OBJECT:
        case XR_KIND_INSTANCE:
        case XR_KIND_CHANNEL:
        case XR_KIND_INTERFACE:
        case XR_KIND_CLASS:
            return XR_SLOT_PTR;
        case XR_KIND_UNION: {
            // If all members map to same slot type, use it; otherwise ANY
            if (type->union_type.member_count == 0)
                return XR_SLOT_ANY;
            uint8_t common = xr_type_to_slot_type(type->union_type.members[0]);
            for (int i = 1; i < type->union_type.member_count; i++) {
                if (xr_type_to_slot_type(type->union_type.members[i]) != common)
                    return XR_SLOT_ANY;
            }
            return common;
        }
        default:
            return XR_SLOT_ANY;
    }
}

// API: Derive precise xr_tag from XrType (for per-PC type annotation).
// Returns XR_TAG_* (0-7), or meta-tags: 0xFC=NUMERIC, 0xFF=UNKNOWN.
// Unlike xr_type_rep() which collapses to 5 reps, this preserves
// value-level distinctions (NULL/BOOL/I64/F64/PTR).
static inline uint8_t xr_type_to_xr_tag(const XrType *t) {
    if (!t)
        return 0xFF;
    if (xr_type_is_enum_metadata(t))
        return XR_TAG_I64;
    if (t->is_nullable) {
        // PTR-based T?: safe to tag as PTR.
        // jit_value_from_tag(0, PTR) returns {tag=NULL} for null values.
        // I64/F64 nullable: payload ambiguous (int(0)/float(0.0) vs null).
        XrRep base = xr_type_base_rep(t);
        return (base == XR_REP_PTR) ? XR_TAG_PTR : 0xFF;
    }
    switch (t->kind) {
        case XR_KIND_INT:
            return XR_TAG_I64;
        case XR_KIND_POINTER:
            return XR_TAG_I64;  // raw pointer = address-width integer
        case XR_KIND_FLOAT:
            return XR_TAG_F64;
        case XR_KIND_BOOL:
            return 1;  // XR_TAG_BOOL: payload 0=false, 1=true
        case XR_KIND_NULL:
        case XR_KIND_UNIT:
            return XR_TAG_NULL;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_JSON:
        case XR_KIND_STRUCT_OBJECT:
        case XR_KIND_INSTANCE:
        case XR_KIND_CHANNEL:
        case XR_KIND_INTERFACE:
        case XR_KIND_CLASS:
        case XR_KIND_FUNCTION:
        case XR_KIND_ENUM:
            return XR_TAG_PTR;
        case XR_KIND_UNION: {
            // If all members share the same tag, return it (e.g. string|array → PTR)
            if (t->union_type.member_count == 0)
                return 0xFF;
            uint8_t common = xr_type_to_xr_tag(t->union_type.members[0]);
            if (common == 0xFF)
                return 0xFF;
            bool all_same = true;
            bool all_numeric = (common == XR_TAG_I64 || common == XR_TAG_F64);
            for (int i = 1; i < t->union_type.member_count; i++) {
                uint8_t mt = xr_type_to_xr_tag(t->union_type.members[i]);
                if (mt != common)
                    all_same = false;
                if (mt != XR_TAG_I64 && mt != XR_TAG_F64)
                    all_numeric = false;
            }
            if (all_same)
                return common;
            if (all_numeric)
                return 0xFC;  // XR_RTAG_NUMERIC: int|float union
            return 0xFF;
        }
        default:
            return 0xFF;
    }
}

// API: Extract element type GC tag from container XrType.
// For Array<T>/Set<T>/Channel<T>, returns gc_tag of T.
// For Map<K,V>, returns gc_tag of V (value type).
// For non-container types, returns XR_SLOT_ANY.
static inline uint8_t xr_type_element_gc_tag(XrType *type) {
    if (!type)
        return XR_SLOT_ANY;
    switch (type->kind) {
        case XR_KIND_ARRAY:
        case XR_KIND_SLICE:
        case XR_KIND_SET:
        case XR_KIND_CHANNEL:
            return xr_type_to_slot_type(type->container.element_type);
        case XR_KIND_MAP:
            return xr_type_to_slot_type(type->map.value_type);
        default:
            return XR_SLOT_ANY;
    }
}

// API: Container types
XR_FUNC XrType *xr_type_new_array(XrVMRuntime *X, XrType *element_type);
XR_FUNC XrType *xr_type_new_slice(XrVMRuntime *X, XrType *element_type);
XR_FUNC XrType *xr_type_new_u8_slice(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_map(XrVMRuntime *X, XrType *key_type, XrType *value_type);
XR_FUNC XrType *xr_type_new_set(XrVMRuntime *X, XrType *element_type);
XR_FUNC XrType *xr_type_new_channel(XrVMRuntime *X, XrType *element_type);
XR_FUNC XrType *xr_type_new_task(XrVMRuntime *X, XrType *result_type);

// API: Object types
XR_FUNC XrType *xr_type_new_json(XrVMRuntime *X);
/* Whether every value represented by this static type is already a member of
 * the Json value domain. This is deliberately narrower than encodability. */
XR_FUNC bool xr_type_is_json_value(const XrType *type);
XR_FUNC XrType *xr_type_new_struct_object_with_fields(XrVMRuntime *X, const char **names,
                                                      XrType **types, int count);
XR_FUNC void xr_type_set_object_field_readonly(XrVMRuntime *X, XrType *type, const bool *readonly,
                                               int count);
XR_FUNC void xr_type_set_object_type_name(XrVMRuntime *X, XrType *type, const char *name);
XR_FUNC void xr_type_set_json_field_readonly(XrVMRuntime *X, XrType *type, const bool *readonly,
                                             int count);
XR_FUNC void xr_type_set_json_type_name(XrVMRuntime *X, XrType *type, const char *name);
XR_FUNC XrType *xr_type_new_class(XrVMRuntime *X, const char *class_name);
XR_FUNC XrType *xr_type_new_interface(XrVMRuntime *X, const char *interface_name);
XR_FUNC XrType *xr_type_new_generic_interface(XrVMRuntime *X, const char *interface_name,
                                              XrType **type_args, int type_arg_count);
XR_FUNC XrType *xr_type_new_instance(XrVMRuntime *X, XrClassInfo *class_info);
XR_FUNC XrType *xr_type_new_generic_instance(XrVMRuntime *X, const char *class_name,
                                             XrClassInfo *class_info, XrType **type_args,
                                             int type_arg_count);
XR_FUNC XrType *xr_type_new_enum_metadata(XrVMRuntime *X, const char *metadata_name,
                                          XrType *enum_type);
XR_FUNC XrType *xr_type_new_bigint(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_u8_array(XrVMRuntime *X);
XR_FUNC XrType *
xr_type_new_named_instance(XrVMRuntime *X,
                           const char *name);  // generic named class (Exception/Range/etc)
XR_FUNC XrType *xr_type_new_enum(XrVMRuntime *X, const char *enum_name);
XR_FUNC XrType *xr_type_new_generic_enum(XrVMRuntime *X, const char *enum_name,
                                         const XrEnumLayout *layout, XrType **type_args,
                                         int type_arg_count);

// API: Optional type (T?)
XR_FUNC XrType *xr_type_new_optional(XrVMRuntime *X, XrType *base_type);
XR_FUNC XrType *xr_type_get_base(XrType *optional_type);

// API: Type parameter (for generics)
XR_FUNC XrType *xr_type_new_type_param(XrVMRuntime *X, const char *name, int id);
XR_FUNC XrType *xr_type_new_type_param_constrained(XrVMRuntime *X, const char *name, int id,
                                                   XrType *constraint);

// API: Function type
XR_FUNC XrType *xr_type_new_function(XrVMRuntime *X, XrType **param_types, int param_count,
                                     XrType *return_type, bool is_variadic);
XR_FUNC void xr_type_set_function_type_params(XrVMRuntime *X, XrType *type, const char **names,
                                              XrType ***constraint_lists,
                                              const int *constraint_counts, int count);

// API: Tuple type (for multi-value return)
XR_FUNC XrType *xr_type_new_tuple(XrVMRuntime *X, XrType **element_types, int count);
XR_FUNC int xr_type_tuple_count(XrType *type);
XR_FUNC XrType *xr_type_tuple_get(XrType *type, int index);

// API: Union type construction and operations
/* Temporary union arrays use the current type pool's request ledger. */
XR_FUNC XrType **xr_type_union_members_alloc(size_t count);
XR_FUNC void xr_type_union_members_free(XrType **members);
XR_FUNC XrType *xr_type_new_union(XrVMRuntime *X, XrType **members, int count);
XR_FUNC XrType *xr_type_union(XrVMRuntime *X, XrType *a, XrType *b);
XR_FUNC int xr_type_union_count(XrType *type);
XR_FUNC XrType *xr_type_union_member(XrType *type, int index);
XR_FUNC bool xr_type_union_contains(XrType *type, XrTypeKind kind);
/* Union members must be tellable apart at run time, because that is all `is` and
 * `match` have to work with. A dynamically erased value keeps only its i64 or
 * f64 family, so two integer members (or two float members) are one runtime
 * type wearing two static names: no value could ever select between them, and
 * no assignment could say which one it stored. Reports the first such pair. */
XR_FUNC bool xr_type_union_indiscriminable_pair(const XrType *type, XrType **out_first,
                                                XrType **out_second);
/* The single member of `type` in the numeric family a literal of `kind`
 * naturally belongs to, or NULL. Integer literals fall back to the float member
 * when the union has no integer one, mirroring the plain contextual rule that
 * types `1` into a float target. */
XR_FUNC XrType *xr_type_union_numeric_member_for_literal(XrType *type, XrTypeKind literal_kind);
XR_FUNC XrType *xr_type_union_remove(XrVMRuntime *X, XrType *type, XrTypeKind kind);

// Whether a type's value domain natively includes null, as a property
// distinct from the `is_nullable` decoration.
//
// `is_nullable` describes the syntactic form `T | null` over a base
// type T that itself does NOT include null — it carries a paired
// non-nullable "base" form reachable via xr_type_non_nullable, and
// printable forms render with a trailing `?`.
//
// "Intrinsically includes null" describes a type whose value domain
// already contains null with no separate non-nullable form. Today
// that is exactly Json: a Json value can be null without any
// optional decoration, and `Json?` is rejected by the parser as
// redundant.
//
// Three places need this distinction:
//   - parser: reject `Json?` (and any future intrinsic-null type)
//   - null-safety analyzer: skip the "null -> non-nullable" error
//   - Json coercion: NULL source flows into a Json sink
//
// Keeping the rule in one helper avoids hard-coding `kind == JSON`
// in three different files.
static inline bool xr_type_intrinsically_includes_null(const XrType *t) {
    return t && t->kind == XR_KIND_JSON;
}

static inline bool xr_type_is_exact_struct_object(const XrType *type) {
    return type && XR_TYPE_IS_STRUCT_OBJECT(type);
}

static inline bool xr_type_object_fields_are_closed(const XrType *type) {
    return XR_TYPE_IS_STRUCT_OBJECT(type);
}

static inline bool xr_type_object_accepts_extra_fields(const XrType *type) {
    (void) type;
    return false;
}

// JSON.Value is a closed data-exchange value domain. Scalars widen implicitly;
// composite Xray values cross through JSON.value so encoding stays explicit.
static inline bool xr_is_json_coercion(XrType *target, XrType *source) {
    if (!target || !source)
        return false;

    if (target->kind == XR_KIND_JSON) {
        switch (source->kind) {
            case XR_KIND_JSON:
            case XR_KIND_UNKNOWN:
            case XR_KIND_NULL:
            case XR_KIND_INT:
            case XR_KIND_FLOAT:
            case XR_KIND_STRING:
            case XR_KIND_BOOL:
                return true;
            default:
                return false;
        }
    }

    if (source->kind != XR_KIND_JSON && source->kind != XR_KIND_UNKNOWN)
        return false;
    if (xr_kind_is_primitive(target->kind))
        return true;
    if (target->kind == XR_KIND_JSON)
        return true;
    // Union of Json-compatible types.
    if (target->kind == XR_KIND_UNION) {
        for (int i = 0; i < target->union_type.member_count; i++) {
            XrType *m = target->union_type.members[i];
            if (!m)
                return false;
            if (!xr_kind_is_primitive(m->kind) && m->kind != XR_KIND_JSON)
                return false;
        }
        return target->union_type.member_count > 0;
    }
    return false;
}

// Check if a type is valid as an already-typed Json literal field value.
// External structural object/Array/Map/Set/class values must cross through Json.encode.
static inline bool xr_type_is_json_field_compatible(XrType *type) {
    if (!type)
        return true;
    switch (type->kind) {
        case XR_KIND_UNKNOWN:
        case XR_KIND_NULL:
        case XR_KIND_JSON:
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_STRING:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
            return true;
        case XR_KIND_UNION:
            for (int i = 0; i < type->union_type.member_count; i++) {
                if (!xr_type_is_json_field_compatible(type->union_type.members[i]))
                    return false;
            }
            return true;
        default:
            return false;
    }
}

// API: Fixed-length array ([T; N] - compile-time length)
XR_FUNC XrType *xr_type_new_fixed_array(XrVMRuntime *X, XrType *element_type, int length);

// API: Type operations
XR_FUNC XrType *xr_type_copy(XrVMRuntime *X, XrType *type);
XR_FUNC bool xr_type_assignable(XrType *target, XrType *source);
XR_FUNC bool xr_type_equals(XrType *a, XrType *b);
/* Explain why `source` is not assignable to `target` when the root cause is a
 * structural object/object-shape field-set difference, possibly nested inside an invariant
 * container. Writes a reason such as "extra field 'z'", "missing field 'y'", or
 * "element: extra field 'z'" into `buf` and returns true. Returns false for
 * scalar or otherwise unstructured mismatches, so callers keep their plain
 * "not assignable" message. Only call after xr_type_assignable() has failed. */
XR_FUNC bool xr_type_object_mismatch_reason(XrType *target, XrType *source, char *buf, size_t n);
// Classify a conversion between concrete numeric types.  This does not grant
// contextual-literal or dynamic-cast semantics; the analyzer upgrades those
// cases using the same XrConversionKind domain.
XR_FUNC XrConversionKind xr_type_numeric_conversion_kind(const XrType *target,
                                                         const XrType *source);
XR_FUNC bool xr_type_numeric_implicitly_convertible(const XrType *target, const XrType *source);
// Returns one operand type when both numeric operands have a unique implicit
// common type, otherwise NULL.  No C usual-arithmetic-conversion fallback.
XR_FUNC XrType *xr_type_numeric_common_type(XrType *left, XrType *right);
/* Exact function signature compatibility with covariant throw effect:
 * NO_THROW implements/overrides MAY_THROW, never the reverse. */
XR_FUNC bool xr_type_function_signature_assignable(XrType *target, XrType *source);
/* Resource aborts preserve the selected pool's original CompileState cause.
 * Resource-aware callers must inspect that cause before interpreting the bind status. */
XR_FUNC XrViewOriginBindStatus xr_type_function_set_view_origins(XrVMRuntime *X, XrType *type,
                                               const XrViewOrigin *origins, int count,
                                               bool was_elided);
XR_FUNC bool xr_type_function_view_origins_equal(const XrType *a, const XrType *b);
XR_FUNC XrViewOriginBindStatus xr_type_function_bind_view_origins(
    XrVMRuntime *X, XrType *type, const char **param_names, XrBorrowOriginSyntaxState syntax,
    const AstBorrowOriginRef *origins, int origin_count, bool has_receiver);

// API: Nullable operations (safe for singletons - copies if frozen)
XR_FUNC XrType *xr_type_make_nullable(XrVMRuntime *X, XrType *type);

// API: Type narrowing
XR_FUNC XrType *xr_type_filter(XrVMRuntime *X, XrType *type, XrTypeKind kind);
XR_FUNC XrType *xr_type_exclude(XrVMRuntime *X, XrType *type, XrTypeKind kind);
XR_FUNC XrType *xr_type_non_nullable(XrVMRuntime *X, XrType *type);

// API: Type utilities
XR_FUNC const char *xr_type_to_string(XrType *type);

// API: Type classification
XR_FUNC bool xr_type_is_inherently_immutable(XrType *type);

// API: Default-initializable check
// A type is default-initializable if a variable of that type can be
// declared without an explicit initializer (e.g. `var x: int`).
// Default-initializable types: numeric primitives, bool, unit, nullable T?,
// and structs where every field is itself default-initializable.
// Non-default-initializable: class instance, string, containers, channel,
// task, function, interface, non-nullable union, struct with non-default fields.
XR_FUNC bool xr_type_is_default_initializable(const XrType *type);

// API: Immutability
XR_FUNC bool xr_type_is_const(XrType *type);
XR_FUNC XrType *xr_type_make_const(XrVMRuntime *X, XrType *base);

// API: Structural compatibility (for JSON objects)
XR_FUNC XrType *xr_type_object_get_field(XrType *type, const char *field_name);

// API: Class inheritance
XR_FUNC bool xr_type_satisfies_constraint(XrType *type, XrType *constraint);

// API: Generic type substitution
// Substitute type parameters with actual types
// e.g., substitute(T, ["T"], [int]) = int
//       substitute(Array<T>, ["T"], [int]) = Array<int>
XR_FUNC XrType *xr_type_substitute(XrVMRuntime *X, XrType *type, const char **param_names,
                                   XrType **actual_types, int count);

// API: Initialize process-level type singletons (call once at startup)
XR_FUNC void xr_type_global_init(void);

// Release process-level type-system state (borrowed current type pool).
// Called via xr_process_shutdown() in test/tool builds. Idempotent.
XR_FUNC void xr_type_global_shutdown(void);

// API: Set current type pool (called by XaAnalyzer before analysis)
// This eliminates global state - each analyzer has its own pool
XR_FUNC void xr_type_set_current_pool(XrTypePool *pool, uint32_t *id_counter);

// Helper accessors for XrType fields
static inline const char *xr_type_get_class_name(XrType *t) {
    if (!t)
        return NULL;
    if (t->kind == XR_KIND_CLASS || t->kind == XR_KIND_INSTANCE) {
        return t->instance.class_name;
    }
    return NULL;
}

static inline XrType *xr_type_get_element(XrType *t) {
    return t ? t->container.element_type : NULL;
}

static inline XrType *xr_type_get_key(XrType *t) {
    return t ? t->map.key_type : NULL;
}

static inline XrType *xr_type_get_value(XrType *t) {
    return t ? t->map.value_type : NULL;
}

static inline XrType *xr_type_get_return(XrType *t) {
    return t ? t->function.return_type : NULL;
}

// Error-effect bit accessor (task 216). Non-function types report MAY_THROW
// (fail-closed): callers only ever skip an error check when a value is proven
// NO_THROW, so an unexpected kind must never masquerade as non-throwing.
static inline XrFnThrowEffect xr_type_function_throw_effect(const XrType *t) {
    if (!t || t->kind != XR_KIND_FUNCTION)
        return XR_FN_EFFECT_MAY_THROW;
    return t->function.throw_effect;
}

static inline bool xr_type_function_is_no_throw(const XrType *t) {
    return t && t->kind == XR_KIND_FUNCTION && t->function.throw_effect == XR_FN_EFFECT_NO_THROW;
}

static inline bool xr_type_function_set_throw_effect(XrType *t, XrFnThrowEffect effect) {
    if (!t || t->kind != XR_KIND_FUNCTION || t->frozen)
        return false;
    t->function.throw_effect = effect;
    return true;
}

static inline int xr_type_get_param_count(XrType *t) {
    return t ? t->function.param_count : 0;
}

static inline XrType *xr_type_function_param_type(const XrType *t, int index) {
    if (!t || t->kind != XR_KIND_FUNCTION || index < 0 || index >= t->function.param_count ||
        !t->function.params)
        return NULL;
    return t->function.params[index].type;
}

static inline XrParamMode xr_type_function_param_mode(const XrType *t, int index) {
    if (!t || t->kind != XR_KIND_FUNCTION || index < 0 || index >= t->function.param_count ||
        !t->function.params)
        return XR_PARAM_READ;
    return t->function.params[index].mode;
}

static inline bool xr_type_function_set_param_mode(XrType *t, int index, XrParamMode mode) {
    if (!t || t->kind != XR_KIND_FUNCTION || index < 0 || index >= t->function.param_count ||
        !t->function.params || !xr_param_mode_is_valid(mode))
        return false;
    t->function.params[index].mode = mode;
    return true;
}

// ============================================================================
// SlotType → XrType* Conversion (for type unification)
// ============================================================================

// Convert XrSlotType to XrType* singleton. Returns NULL for ANY/unknown.
// Singletons are global read-only; X is passed for API consistency.
static inline XrType *xr_slot_type_to_type(XrVMRuntime *X, uint8_t slot_type) {
    switch (slot_type) {
        case XR_SLOT_I64:
            return xr_type_new_int(X);
        case XR_SLOT_F64:
            return xr_type_new_float(X);
        case XR_SLOT_BOOL:
            return xr_type_new_bool(X);
        case XR_SLOT_PTR:
            return xr_type_new_string(X);  // generic heap ref (best approx)
        default:
            return NULL;
    }
}

// ============================================================================
// Iterable/Iterator Structural Type Checking
// ============================================================================

// Check if type satisfies Iterator<T> (has hasNext(): bool and next(): T)
// out_element_type receives the element type (return type of next())
XR_FUNC bool xr_type_is_iterator(XrType *type, XrType **out_element_type);

// Check if type satisfies Iterable<T> (built-in or has iterator() -> Iterator<T>)
// out_element_type receives the element type
XR_FUNC bool xr_type_is_iterable(XrType *type, XrType **out_element_type);

/* Json.decode first-stage field contract. ANY means the type is unsupported,
 * not that validation may be skipped. Supported structural leaves include nested
 * object descriptors and Array<Json> containers. */
XR_FUNC uint8_t xr_type_json_value_kind(const XrType *type);
XR_FUNC uint64_t xr_type_stable_key(const XrType *type);
XR_FUNC bool xr_type_is_json_decode_field_supported(const XrType *type);

#endif  // XTYPE_H
