/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype.h - Unified type system definitions
 *
 * KEY CONCEPT:
 *   Each XrType has exactly one XrTypeKind.
 *   Supports union types (int | string) and T? (nullable) via is_nullable flag.
 *   Category checks (numeric, primitive, etc.) use inline functions.
 */

#ifndef XTYPE_H
#define XTYPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xvalue.h"
#include "xslot_type.h"
#include "xstruct_layout.h"
#include "../../base/xdefs.h"

/* ========== XrRep - Machine Representation ========== */
/*
 * Replaces XmType. Derived from XrType via xr_type_rep().
 * Only 5 values — no value-level hints (NULL/TRUE/FALSE are xr_tag).
 */
typedef enum {
    XR_REP_I64 = 0,     // 64-bit integer (raw, untagged)
    XR_REP_F64 = 1,     // 64-bit float (raw, untagged)
    XR_REP_PTR = 2,     // GC pointer (needs barrier/scanning)
    XR_REP_TAGGED = 3,  // full 16B XrValue (tag + payload)
    XR_REP_VOID = 4,    // no value
    XR_REP_STR = 5,     // NUL-terminated C string (AOT only, no GC)
} XrRep;

/* ========== XrType - Static type system ========== */

// Forward declaration
typedef struct XrTypePool XrTypePool;

// Primary type discriminator - each XrType has exactly one kind.
// Supports union types (int | string) and T? (nullable).
typedef enum XrTypeKind {
    XR_KIND_INT = 0,
    XR_KIND_FLOAT,
    XR_KIND_STRING,
    XR_KIND_BOOL,
    XR_KIND_NULL,
    XR_KIND_ARRAY,
    XR_KIND_MAP,
    XR_KIND_SET,
    XR_KIND_CHANNEL,
    XR_KIND_JSON,
    XR_KIND_CLASS,
    XR_KIND_INSTANCE,
    XR_KIND_INTERFACE,
    XR_KIND_FUNCTION,
    XR_KIND_UNKNOWN,
    XR_KIND_NEVER,
    XR_KIND_VOID,
    XR_KIND_ENUM,
    XR_KIND_TYPE_PARAM,
    XR_KIND_TUPLE,
    XR_KIND_UNION,        // Union type: int | string (compile-time only)
    XR_KIND_FIXED_ARRAY,  // Fixed-length array: [N]T (compile-time length, runtime Array<T>)
    XR_KIND_COUNT
} XrTypeKind;

// Category checking inline functions (replace old bit-flag combinations)
static inline bool xr_kind_is_numeric(XrTypeKind k) {
    return k == XR_KIND_INT || k == XR_KIND_FLOAT;
}
static inline bool xr_kind_is_primitive(XrTypeKind k) {
    return k == XR_KIND_INT || k == XR_KIND_FLOAT || k == XR_KIND_STRING || k == XR_KIND_BOOL;
}
static inline bool xr_kind_is_container(XrTypeKind k) {
    return k == XR_KIND_ARRAY || k == XR_KIND_MAP || k == XR_KIND_SET;
}
static inline bool xr_kind_is_builtin_iterable(XrTypeKind k) {
    return k == XR_KIND_ARRAY || k == XR_KIND_MAP || k == XR_KIND_SET || k == XR_KIND_STRING;
}
static inline bool xr_kind_is_object_like(XrTypeKind k) {
    return k == XR_KIND_JSON || k == XR_KIND_INSTANCE || k == XR_KIND_MAP;
}

// Function parameter passing modes (used by XrType.function.param_passing_modes)
#ifndef XR_PARAM_VALUE
#define XR_PARAM_VALUE 0  // Default: deep copy at function entry
#define XR_PARAM_IN 1     // Readonly reference: no copy, no mutation allowed
#define XR_PARAM_REF 2    // Mutable reference: no copy, mutation visible to caller
#endif

// Forward declarations
typedef struct XrType XrType;
typedef struct XrClassInfo XrClassInfo;

// Object type: unified Json with optional compile-time field info.
// is_sealed=true means fixed fields (no runtime extension).
typedef struct XrObjectType {
    const char **field_names;  // Field names array
    XrType **field_types;      // Field types array (parallel to names)
    bool *field_readonly;      // Per-field readonly flags (optional)
    int field_count;           // Number of fields
    const char *type_name;     // NULL for anonymous, name for type alias
    bool is_sealed;            // true = fixed fields, false = extensible at runtime
} XrObjectType;

// Type structure
struct XrType {
    XrTypeKind kind;  // Primary type discriminator
    uint32_t id;      // Unique type ID for caching
    bool frozen;      // Singleton protection

    union {
        // For Array<T>, Set<T>, Channel<T>
        struct {
            XrType *element_type;
        } container;

        // For Map<K, V>
        struct {
            XrType *key_type;
            XrType *value_type;
        } map;

        // For JSON/object types with structured fields
        XrObjectType object;

        // For class instance
        struct {
            const char *class_name;
            XrClassInfo *class_ref;
            XrType *superclass;  // For inheritance chain
            XrType **type_args;  // Generic type arguments (e.g., Box<int> -> [int])
            int type_arg_count;  // Number of type arguments
        } instance;

        // For function type
        struct {
            XrType **param_types;
            int param_count;
            int min_params;  // Minimum required params (for default params)
            XrType *return_type;
            uint8_t *param_passing_modes;  // NULL or [param_count] of XR_PARAM_*
            bool is_variadic;
        } function;

        // For literal types
        struct {
            union {
                const char *str_value;
                int64_t int_value;
                double float_value;
                bool bool_value;
            };
        } literal;

        // For type parameter (generics)
        struct {
            const char *name;    // Parameter name (e.g., "T")
            int id;              // Unique ID within function/class
            XrType *constraint;  // e.g., <T: Comparable>
        } type_param;

        // For tuple type (multi-value return)
        struct {
            XrType **element_types;
            int element_count;
        } tuple;

        // For enum value type
        struct {
            const char *enum_name;
        } enum_type;

        // For union type (int | string) - compile-time only
        struct {
            XrType **members;      // Flat member types (sorted by kind)
            uint8_t member_count;  // Number of members (≤ XR_UNION_MAX_MEMBERS)
        } union_type;

        // For fixed-length array ([N]T)
        struct {
            XrType *element_type;  // Element type
            int length;            // Fixed length N
        } fixed_array;
    };

    // Type modifiers
    bool is_nullable;    // T | null (shorthand for T?)
    bool is_const;       // Deep immutability (for coroutine safety)
    bool is_value_type;  // Struct value type (copy-on-assign)
    bool is_literal;     // Literal type: kind + literal union holds value
    bool is_weak;        // Weak variant: WeakMap (kind==MAP) / WeakSet (kind==SET)

    // Native width for int/float types (XrSlotType value)
    // 0 = default (int=int64, float=float64), nonzero = specific width
    uint8_t native_width;

    // Type alias name (NULL unless resolved through a type alias)
    const char *alias_name;
};

// Named instance class check: used to identify BIGINT/REGEX/etc after INSTANCE merge
static inline bool xr_type_is_named_class(const XrType *t, const char *name) {
    if (!t || t->kind != XR_KIND_INSTANCE)
        return false;
    return t->instance.class_name && (strcmp(t->instance.class_name, name) == 0);
}

// Type checking macros
#define XR_TYPE_IS_INT(t) ((t)->kind == XR_KIND_INT)
#define XR_TYPE_IS_FLOAT(t) ((t)->kind == XR_KIND_FLOAT)
#define XR_TYPE_IS_STRING(t) ((t)->kind == XR_KIND_STRING)
#define XR_TYPE_IS_BOOL(t) ((t)->kind == XR_KIND_BOOL)
#define XR_TYPE_IS_NULL(t) ((t)->kind == XR_KIND_NULL)
#define XR_TYPE_IS_NUMERIC(t) (xr_kind_is_numeric((t)->kind))
#define XR_TYPE_IS_PRIMITIVE(t) (xr_kind_is_primitive((t)->kind))
#define XR_TYPE_IS_ARRAY(t) ((t)->kind == XR_KIND_ARRAY)
#define XR_TYPE_IS_MAP(t) ((t)->kind == XR_KIND_MAP)
#define XR_TYPE_IS_SET(t) ((t)->kind == XR_KIND_SET)
#define XR_TYPE_IS_FUNCTION(t) ((t)->kind == XR_KIND_FUNCTION)
#define XR_TYPE_IS_INSTANCE(t) ((t)->kind == XR_KIND_INSTANCE)
#define XR_TYPE_IS_UNKNOWN(t) ((t)->kind == XR_KIND_UNKNOWN)
#define XR_TYPE_IS_NEVER(t) ((t)->kind == XR_KIND_NEVER)
#define XR_TYPE_IS_VOID(t) ((t)->kind == XR_KIND_VOID)
#define XR_TYPE_IS_CLASS(t) ((t)->kind == XR_KIND_CLASS)
#define XR_TYPE_IS_INTERFACE(t) ((t)->kind == XR_KIND_INTERFACE)
#define XR_TYPE_IS_NULLABLE(t) ((t)->is_nullable || ((t)->kind == XR_KIND_NULL))
#define XR_TYPE_IS_JSON(t) ((t)->kind == XR_KIND_JSON)
#define XR_TYPE_IS_TYPE_PARAM(t) ((t)->kind == XR_KIND_TYPE_PARAM)
#define XR_TYPE_IS_TUPLE(t) ((t)->kind == XR_KIND_TUPLE)
#define XR_TYPE_IS_OPTIONAL(t) ((t)->is_nullable)
#define XR_TYPE_IS_ENUM(t) ((t)->kind == XR_KIND_ENUM)
#define XR_TYPE_IS_UNION(t) ((t)->kind == XR_KIND_UNION)

#define XR_UNION_MAX_MEMBERS 6

// Derive base rep ignoring nullable flag (for nullable optimization)
static inline XrRep xr_type_base_rep(const XrType *t) {
    if (!t)
        return XR_REP_TAGGED;
    switch (t->kind) {
        case XR_KIND_INT:
        case XR_KIND_BOOL:
        case XR_KIND_NULL:
            return XR_REP_I64;
        case XR_KIND_FLOAT:
            return XR_REP_F64;
        case XR_KIND_VOID:
            return XR_REP_VOID;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_JSON:
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
        // If all members share the same rep, use it; otherwise TAGGED
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
XR_FUNC XrType *xr_type_new(XrayIsolate *X, XrTypeKind kind);
XR_FUNC XrType *xr_type_new_int(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_float(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_string(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_bool(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_null(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_unknown(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_never(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_void(XrayIsolate *X);

// API: Native-width types (int8/16/32/64, uint8/16/32/64, float32/64)
XR_FUNC XrType *xr_type_new_int_width(XrayIsolate *X, int width);    // XrNativeType value
XR_FUNC XrType *xr_type_new_float_width(XrayIsolate *X, int width);  // XrNativeType value

// API: Derive XrSlotType from XrType (P0 unified type pipeline)
// Returns the storage slot type — used by GC scanning, JIT guards, and AOT thunks.
// native_width stores XrNativeType; widen all int variants to I64, float variants to F64.
static inline uint8_t xr_type_to_slot_type(XrType *type) {
    if (!type)
        return XR_SLOT_ANY;
    if (type->native_width != 0 && !type->is_nullable) {
        uint8_t nw = type->native_width;
        if (nw == XR_NATIVE_F32 || nw == XR_NATIVE_F64)
            return XR_SLOT_F64;
        if (nw == XR_NATIVE_BOOL)
            return XR_SLOT_BOOL;
        return XR_SLOT_I64;  // I8/U8/I16/U16/I32/U32/I64/U64 all widen to I64
    }
    switch (type->kind) {
        case XR_KIND_INT:
            return XR_SLOT_I64;
        case XR_KIND_FLOAT:
            return XR_SLOT_F64;
        case XR_KIND_BOOL:
            return XR_SLOT_BOOL;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_JSON:
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
        case XR_KIND_FLOAT:
            return XR_TAG_F64;
        case XR_KIND_BOOL:
            return 1;  // XR_TAG_BOOL: payload 0=false, 1=true
        case XR_KIND_NULL:
            return XR_TAG_NULL;
        case XR_KIND_STRING:
        case XR_KIND_ARRAY:
        case XR_KIND_MAP:
        case XR_KIND_SET:
        case XR_KIND_JSON:
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
XR_FUNC XrType *xr_type_new_array(XrayIsolate *X, XrType *element_type);
XR_FUNC XrType *xr_type_new_map(XrayIsolate *X, XrType *key_type, XrType *value_type);
XR_FUNC XrType *xr_type_new_set(XrayIsolate *X, XrType *element_type);
XR_FUNC XrType *xr_type_new_channel(XrayIsolate *X, XrType *element_type);
XR_FUNC XrType *xr_type_new_task(XrayIsolate *X, XrType *result_type);

// API: Object types
XR_FUNC XrType *xr_type_new_json(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_json_with_fields(XrayIsolate *X, const char **names, XrType **types,
                                             int count, bool is_sealed);
XR_FUNC XrType *xr_type_new_class(XrayIsolate *X, const char *class_name);
XR_FUNC XrType *xr_type_new_interface(XrayIsolate *X, const char *interface_name);
XR_FUNC XrType *xr_type_new_instance(XrayIsolate *X, XrClassInfo *class_info);
XR_FUNC XrType *xr_type_new_generic_instance(XrayIsolate *X, const char *class_name,
                                             XrClassInfo *class_info, XrType **type_args,
                                             int type_arg_count);
XR_FUNC XrType *xr_type_new_bigint(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_datetime(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_bytes(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_regex(XrayIsolate *X);
XR_FUNC XrType *xr_type_new_stringbuilder(XrayIsolate *X);
XR_FUNC XrType *
xr_type_new_named_instance(XrayIsolate *X,
                           const char *name);  // generic named class (Exception/Range/etc)
XR_FUNC XrType *xr_type_new_enum(XrayIsolate *X, const char *enum_name);

// API: Optional type (T?)
XR_FUNC XrType *xr_type_new_optional(XrayIsolate *X, XrType *base_type);
XR_FUNC XrType *xr_type_get_base(XrType *optional_type);

// API: Type parameter (for generics)
XR_FUNC XrType *xr_type_new_type_param(XrayIsolate *X, const char *name, int id);
XR_FUNC XrType *xr_type_new_type_param_constrained(XrayIsolate *X, const char *name, int id,
                                                   XrType *constraint);

// API: Function type
XR_FUNC XrType *xr_type_new_function(XrayIsolate *X, XrType **param_types, int param_count,
                                     XrType *return_type, bool is_variadic);

// API: Tuple type (for multi-value return)
XR_FUNC XrType *xr_type_new_tuple(XrayIsolate *X, XrType **element_types, int count);
XR_FUNC int xr_type_tuple_count(XrType *type);
XR_FUNC XrType *xr_type_tuple_get(XrType *type, int index);

// API: Union type construction and operations
XR_FUNC XrType *xr_type_new_union(XrayIsolate *X, XrType **members, int count);
XR_FUNC XrType *xr_type_union(XrayIsolate *X, XrType *a, XrType *b);
XR_FUNC int xr_type_union_count(XrType *type);
XR_FUNC XrType *xr_type_union_member(XrType *type, int index);
XR_FUNC bool xr_type_union_contains(XrType *type, XrTypeKind kind);
XR_FUNC XrType *xr_type_union_remove(XrayIsolate *X, XrType *type, XrTypeKind kind);

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

// Check if a coercion through Json is allowed at compile time. Returns
// true when the type checker should treat `target = source` as legal,
// possibly with a runtime OP_CHECKTYPE inserted by the codegen.
//
// Two directions are recognised — both routine in xray:
//
//   1. Json -> X. A `Json` source flows into a more specific target
//      (primitive, container, instance, or a union of the above).
//      Codegen inserts OP_CHECKTYPE so a wrong runtime shape becomes
//      a clean exception instead of silent UB.
//
//   2. X -> Json. A typed value is fed into a Json sink. The runtime
//      already stores every kind of value inside the same XrValue
//      tagged union, so this is a label-only coercion with zero work.
//
// The coercion is intentionally permissive at compile time. Anything
// that survives this check still goes through xa_typecheck_assignable
// downstream when the constraints are tighter.
static inline bool xr_is_json_coercion(XrType *target, XrType *source) {
    if (!target || !source)
        return false;

    // Direction 2: any structured value flows into a Json sink. The
    // sink is a dynamic object, so accepting an instance / array / map
    // / set / channel / unknown / null here does not lose
    // information. Json self-contains null, so `null` and the
    // analyzer's `unknown` placeholder are deliberately accepted —
    // forcing a manual `Json?` annotation would be redundant noise.
    if (target->kind == XR_KIND_JSON) {
        switch (source->kind) {
            case XR_KIND_INSTANCE:
            case XR_KIND_ARRAY:
            case XR_KIND_MAP:
            case XR_KIND_SET:
            case XR_KIND_CHANNEL:
            case XR_KIND_JSON:
            case XR_KIND_UNKNOWN:
            case XR_KIND_NULL:
                return true;
            default:
                break;
        }
        if (xr_kind_is_primitive(source->kind))
            return true;
    }

    // Direction 1: Json or unknown source flowing into a more specific target.
    // Unknown typically arises from Json field chains (e.g. node.val + ...)
    // where the analyzer cannot determine a precise type. Codegen inserts
    // runtime OP_CHECKTYPE, so this is safe.
    if (source->kind != XR_KIND_JSON && source->kind != XR_KIND_UNKNOWN)
        return false;
    if (xr_kind_is_primitive(target->kind))
        return true;
    if (target->kind == XR_KIND_JSON)
        return true;
    if (target->kind == XR_KIND_ARRAY)
        return true;
    // Json -> Instance: a Json source feeding a typed instance
    // target, e.g. `let v: SomeClass = json_obj["key"]`. Codegen
    // inserts a runtime type check just like for primitives.
    if (target->kind == XR_KIND_INSTANCE)
        return true;
    // Union of Json-compatible types.
    if (target->kind == XR_KIND_UNION) {
        for (int i = 0; i < target->union_type.member_count; i++) {
            XrType *m = target->union_type.members[i];
            if (!m)
                return false;
            if (!xr_kind_is_primitive(m->kind) && m->kind != XR_KIND_JSON &&
                m->kind != XR_KIND_ARRAY && m->kind != XR_KIND_INSTANCE)
                return false;
        }
        return target->union_type.member_count > 0;
    }
    return false;
}

// Check if a type is valid as a Json field value.
// Rejects types that clearly don't belong in JSON: function, class, channel, interface.
// Allows: primitives, null, Json, Array, Map, Enum, DateTime, unknown, etc.
// Enum serializes as its member name string; DateTime as ISO 8601 string.
static inline bool xr_type_is_json_field_compatible(XrType *type) {
    if (!type)
        return true;
    switch (type->kind) {
        case XR_KIND_FUNCTION:
        case XR_KIND_CLASS:
        case XR_KIND_CHANNEL:
        case XR_KIND_INTERFACE:
            return false;
        case XR_KIND_ENUM:
            return true;
        case XR_KIND_INSTANCE:
            // DateTime is serializable (ISO 8601 string)
            if (type->instance.class_name && strcmp(type->instance.class_name, "DateTime") == 0)
                return true;
            return false;
        case XR_KIND_UNION:
            for (int i = 0; i < type->union_type.member_count; i++) {
                if (!xr_type_is_json_field_compatible(type->union_type.members[i]))
                    return false;
            }
            return true;
        default:
            return true;
    }
}

// API: Fixed-length array ([N]T - compile-time length, runtime Array<T>)
XR_FUNC XrType *xr_type_new_fixed_array(XrayIsolate *X, XrType *element_type, int length);

// API: Type operations
XR_FUNC XrType *xr_type_copy(XrayIsolate *X, XrType *type);
XR_FUNC bool xr_type_assignable(XrType *target, XrType *source);
XR_FUNC bool xr_type_equals(XrType *a, XrType *b);

// API: Nullable operations (safe for singletons - copies if frozen)
XR_FUNC XrType *xr_type_make_nullable(XrayIsolate *X, XrType *type);

// API: Type narrowing
XR_FUNC XrType *xr_type_filter(XrayIsolate *X, XrType *type, XrTypeKind kind);
XR_FUNC XrType *xr_type_exclude(XrayIsolate *X, XrType *type, XrTypeKind kind);
XR_FUNC XrType *xr_type_non_nullable(XrayIsolate *X, XrType *type);

// API: Type utilities
XR_FUNC const char *xr_type_to_string(XrType *type);

// API: Type classification
XR_FUNC bool xr_type_is_inherently_immutable(XrType *type);

// API: Immutability
XR_FUNC bool xr_type_is_const(XrType *type);
XR_FUNC XrType *xr_type_make_const(XrayIsolate *X, XrType *base);

// API: Structural compatibility (for JSON objects)
XR_FUNC XrType *xr_type_object_get_field(XrType *type, const char *field_name);

// API: Class inheritance
XR_FUNC bool xr_type_satisfies_constraint(XrType *type, XrType *constraint);

// API: Generic type substitution
// Substitute type parameters with actual types
// e.g., substitute(T, ["T"], [int]) = int
//       substitute(Array<T>, ["T"], [int]) = Array<int>
XR_FUNC XrType *xr_type_substitute(XrayIsolate *X, XrType *type, const char **param_names,
                                   XrType **actual_types, int count);

// API: Initialize process-level type singletons (call once at startup)
XR_FUNC void xr_type_global_init(void);

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

static inline int xr_type_get_param_count(XrType *t) {
    return t ? t->function.param_count : 0;
}

static inline XrType **xr_type_get_param_types(XrType *t) {
    return t ? t->function.param_types : NULL;
}

// ============================================================================
// SlotType → XrType* Conversion (for JIT feedback → type unification)
// ============================================================================

// Convert XrSlotType to XrType* singleton. Returns NULL for ANY/unknown.
// Singletons are global read-only; X is passed for API consistency.
static inline XrType *xr_slot_type_to_type(XrayIsolate *X, uint8_t slot_type) {
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

#endif  // XTYPE_H
