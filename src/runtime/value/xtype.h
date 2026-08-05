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
#include "xenum_layout.h"
#include "../../base/xdefs.h"
#include "../../shared/xr_param_mode.h"
#include "../../shared/xr_json_type.h"
#include "../../shared/xr_scalar_type.h"
#include "../../shared/xr_conversion.h"
#include "../../shared/xobject_row.h"

/* ========== XrRep - Machine Representation ========== */
/*
 * Derived from XrType via xr_type_rep().
 * Value-level hints such as NULL/TRUE/FALSE are xr_tag; reps describe native
 * storage selected by IR/AOT.
 */
typedef enum {
    XR_REP_I64 = 0,     // 64-bit integer (raw, untagged)
    XR_REP_F64 = 1,     // 64-bit float (raw, untagged)
    XR_REP_PTR = 2,     // GC pointer (needs barrier/scanning)
    XR_REP_TAGGED = 3,  // full 16B XrValue (tag + payload)
    XR_REP_VOID = 4,    // no value
    XR_REP_STR = 5,     // NUL-terminated C string (AOT only, no GC)
    XR_REP_RAWPTR = 6,  // raw C pointer address (AOT only, GC-invisible)
    XR_REP_COUNT = 7,
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
    XR_KIND_ERROR,  // Compiler-only recovery poison; never a source type.
    XR_KIND_NEVER,
    XR_KIND_UNIT,  // Unit (0-arity tuple): canonical "no meaningful value" type
    XR_KIND_ENUM,
    XR_KIND_TYPE_PARAM,
    XR_KIND_TUPLE,
    XR_KIND_UNION,        // Union type: int | string (compile-time only)
    XR_KIND_FIXED_ARRAY,  // Fixed-length array: [T; N] (compile-time length)
    XR_KIND_POINTER,      // FFI raw pointer: Ptr<T> (const) / MutPtr<T> (mut). Address-
                          // width integer at the value level, invisible to the GC.
    XR_KIND_RUNE,         // Unicode scalar value. Immediate value (tag XR_TAG_RUNE), not
                          // a uint32; appended last to keep existing kind values stable.
    XR_KIND_RECORD,       // Sealed/open structural record; shares ObjectShape metadata with Json.
    XR_KIND_SLICE,        // Borrowed contiguous view; source surface: Slice<T>.
    XR_KIND_COUNT
} XrTypeKind;

// Category checking inline functions (replace old bit-flag combinations)
static inline bool xr_kind_is_numeric(XrTypeKind k) {
    return k == XR_KIND_INT || k == XR_KIND_FLOAT;
}
static inline bool xr_kind_is_primitive(XrTypeKind k) {
    return k == XR_KIND_INT || k == XR_KIND_FLOAT || k == XR_KIND_STRING || k == XR_KIND_BOOL ||
           k == XR_KIND_RUNE;
}
static inline bool xr_kind_is_container(XrTypeKind k) {
    return k == XR_KIND_ARRAY || k == XR_KIND_SLICE || k == XR_KIND_MAP || k == XR_KIND_SET;
}
static inline bool xr_kind_is_builtin_iterable(XrTypeKind k) {
    return k == XR_KIND_ARRAY || k == XR_KIND_SLICE || k == XR_KIND_MAP || k == XR_KIND_SET ||
           k == XR_KIND_STRING;
}
static inline bool xr_kind_has_object_shape(XrTypeKind k) {
    return k == XR_KIND_RECORD || k == XR_KIND_JSON;
}
static inline bool xr_kind_is_object_like(XrTypeKind k) {
    return xr_kind_has_object_shape(k) || k == XR_KIND_INSTANCE || k == XR_KIND_MAP;
}

// Forward declarations
typedef struct XrType XrType;
typedef struct XrClassInfo XrClassInfo;

#define XR_ENUM_VARIANTS_TYPE_NAME "EnumVariants"
#define XR_ENUM_VARIANT_TYPE_NAME "EnumVariant"
#define XR_ENUM_PAYLOADS_TYPE_NAME "EnumPayloads"
#define XR_ENUM_PAYLOAD_FIELD_TYPE_NAME "EnumPayloadField"

/* Stable runtime kind carried by an erased enum-domain descriptor box.  Keep
 * this independent from frontend selection ids: it is part of the VM/AOT
 * representation contract, not a source-level reflection API. */
typedef enum XrEnumMetadataKind {
    XR_ENUM_METADATA_NONE = 0,
    XR_ENUM_METADATA_VARIANTS = 1,
    XR_ENUM_METADATA_VARIANT = 2,
    XR_ENUM_METADATA_PAYLOADS = 3,
    XR_ENUM_METADATA_PAYLOAD_FIELD = 4,
} XrEnumMetadataKind;

typedef struct XrFunctionParam {
    XrType *type;
    XrParamMode mode;
} XrFunctionParam;

// Error-effect "may-throw" bit carried by the internal function type (task 216).
//
// This is a TYPED dimension of a function type — the 1-bit "is this function
// allowed to raise into the error channel?" — kept strictly separate from the
// error *set* (which enum variants), which stays inferred in XaEffectDatabase
// and never enters the type system. The bit participates in code generation
// (constructive ERR_CHECK emission), canonical no-throw constraints, and HOF effect
// polymorphism.
//
// Storage default is fail-closed MAY_THROW: any function type produced without
// an authoritative effect conclusion is treated as possibly throwing. The
// analyzer downgrades a definition to NO_THROW only after the effect-DB
// fixpoint proves its summary is complete AND its escaping set is empty.
typedef enum XrFnThrowEffect {
    XR_FN_EFFECT_NO_THROW = 0,   // proven not to throw (complete ∧ empty escaping)
    XR_FN_EFFECT_MAY_THROW = 1,  // may throw, or incomplete/unknown (fail-closed)
    XR_FN_EFFECT_POLY = 2,       // effect variable: parameter-position function types
                                 // (rethrows — instantiated per call site by the
                                 // effect of the actual argument)
} XrFnThrowEffect;

/* Borrowed Slice return provenance is part of a function signature even
 * though Xray deliberately has no lifetime-annotation syntax.  A definition
 * may publish one parameter or the receiver as the unique backing source;
 * UNKNOWN/MULTI are fail-closed states and are never usable as a safe return
 * contract. */
typedef enum XrViewReturnSourceKind {
    XR_VIEW_RETURN_NONE = 0,
    XR_VIEW_RETURN_PARAM,
    XR_VIEW_RETURN_RECEIVER,
    XR_VIEW_RETURN_STATIC,
    XR_VIEW_RETURN_LOCAL,
    XR_VIEW_RETURN_MULTI,
    XR_VIEW_RETURN_UNKNOWN,
} XrViewReturnSourceKind;

// Object-shape metadata shared by structural and Json object types. Row
// compatibility and runtime Json extension are deliberately orthogonal.
typedef struct XrObjectType {
    const char **field_names;  // Field names array
    XrType **field_types;      // Field types array (parallel to names)
    bool *field_readonly;      // Per-field readonly flags (optional)
    int field_count;           // Number of fields
    const char *type_name;     // NULL for anonymous, name for type alias
    XrObjectRowMode row_mode;       // structural assignment relation
    bool allows_runtime_extension;  // Json-only dynamic field transition
} XrObjectType;

// Type structure
struct XrType {
    XrTypeKind kind;            // Primary type discriminator
    uint32_t id;                // Unique type ID for caching
    uint32_t semantic_type_id;  // Stable analyzer semantic identity; 0 for ordinary types.
    bool frozen;                // Singleton protection

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
            XrFunctionParam *params;
            int param_count;
            int min_params;  // Minimum required params (for default params)
            XrType *return_type;
            bool is_variadic;
            bool is_c_abi;  // C function pointer ABI (`CFn<...>`), no Xray closure header
            // Error-effect "may-throw" bit (task 216). Default is fail-closed
            // MAY_THROW; the analyzer proves NO_THROW after the effect-DB
            // fixpoint. POLY marks parameter-position (rethrows) function types.
            XrFnThrowEffect throw_effect;
            XrViewReturnSourceKind view_return_source;
            int16_t view_return_param;  // valid only for XR_VIEW_RETURN_PARAM
            bool view_return_complete;
            const char **type_param_names;
            XrType ***type_param_constraints;
            int *type_param_constraint_counts;
            int type_param_count;
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
            uint32_t layout_id;
            const XrEnumLayout *layout;
            XrType **type_args;  // Concrete arguments retained by a generic enum type domain.
            int type_arg_count;
        } enum_type;

        // For union type (int | string) - compile-time only
        struct {
            XrType **members;      // Flat member types (sorted by kind)
            uint8_t member_count;  // Number of members (≤ XR_UNION_MAX_MEMBERS)
        } union_type;

        // For fixed-length array ([T; N])
        struct {
            XrType *element_type;  // Element type
            int length;            // Fixed length N
        } fixed_array;
    };

    // Type modifiers
    bool is_nullable;         // T | null (shorthand for T?)
    bool is_const;            // Deep immutability (for coroutine safety)
    bool is_value_type;       // Struct value type (copy-on-assign)
    bool is_literal;          // Literal type: kind + literal union holds value
    bool is_cycle_candidate;  // Class type graph forms a cycle (RC cycle collector)
    bool ptr_is_mut;          // POINTER only: MutPtr<T> (true) vs Ptr<T> (false, const)

    // Semantic scalar representation. Numeric kinds always carry a valid
    // XrNativeType, including explicit I64/F64; non-numeric kinds do not read it.
    uint8_t scalar_rep;

    // Type alias name (NULL unless resolved through a type alias)
    const char *alias_name;
};

/* The four enum-reflection view types. All are writable annotations over a
 * concrete enum (EnumVariants<Color>, EnumVariant<Color>, ...); they are
 * registered together in stdlib/prelude/builtin_symbols.def. */
static inline bool xr_type_is_enum_metadata_type_name(const char *name) {
    return name && (strcmp(name, XR_ENUM_VARIANTS_TYPE_NAME) == 0 ||
                    strcmp(name, XR_ENUM_VARIANT_TYPE_NAME) == 0 ||
                    strcmp(name, XR_ENUM_PAYLOADS_TYPE_NAME) == 0 ||
                    strcmp(name, XR_ENUM_PAYLOAD_FIELD_TYPE_NAME) == 0);
}

static inline bool xr_type_is_enum_metadata(const XrType *t) {
    if (!t || t->kind != XR_KIND_INSTANCE || !t->instance.class_name)
        return false;
    return xr_type_is_enum_metadata_type_name(t->instance.class_name);
}

static inline bool xr_type_is_enum_metadata_named(const XrType *t, const char *name) {
    return xr_type_is_enum_metadata(t) && name && strcmp(t->instance.class_name, name) == 0;
}

static inline XrType *xr_type_enum_metadata_owner(const XrType *t) {
    return xr_type_is_enum_metadata(t) && t->instance.type_arg_count == 1 && t->instance.type_args
               ? t->instance.type_args[0]
               : NULL;
}

static inline XrEnumMetadataKind xr_type_enum_metadata_kind(const XrType *t) {
    if (!xr_type_is_enum_metadata(t))
        return XR_ENUM_METADATA_NONE;
    const char *name = t->instance.class_name;
    if (strcmp(name, XR_ENUM_VARIANTS_TYPE_NAME) == 0)
        return XR_ENUM_METADATA_VARIANTS;
    if (strcmp(name, XR_ENUM_VARIANT_TYPE_NAME) == 0)
        return XR_ENUM_METADATA_VARIANT;
    if (strcmp(name, XR_ENUM_PAYLOADS_TYPE_NAME) == 0)
        return XR_ENUM_METADATA_PAYLOADS;
    if (strcmp(name, XR_ENUM_PAYLOAD_FIELD_TYPE_NAME) == 0)
        return XR_ENUM_METADATA_PAYLOAD_FIELD;
    return XR_ENUM_METADATA_NONE;
}

static inline uint32_t xr_type_enum_metadata_layout_id(const XrType *t) {
    XrType *owner = xr_type_enum_metadata_owner(t);
    if (!owner || owner->kind != XR_KIND_ENUM)
        return 0;
    if (owner->enum_type.layout && owner->enum_type.layout->layout_id != 0)
        return owner->enum_type.layout->layout_id;
    return owner->enum_type.layout_id;
}

static inline int64_t xr_type_enum_metadata_token(const XrType *t) {
    return ((int64_t) xr_type_enum_metadata_layout_id(t) << 8) |
           (int64_t) xr_type_enum_metadata_kind(t);
}

// Named instance class check: used to identify BIGINT/REGEX/etc after INSTANCE merge
static inline bool xr_type_is_named_class(const XrType *t, const char *name) {
    if (!t || t->kind != XR_KIND_INSTANCE)
        return false;
    return t->instance.class_name && (strcmp(t->instance.class_name, name) == 0);
}

/* Same check, but only for the *builtin* class of that name — never a user
 * class that reuses it. Builtin type names are ordinary identifiers rather
 * than keywords, so `class Range { ... }` is legal source. A name-only test
 * hands such a class the builtin's typing and lowering, which turns what
 * should be a compile error ("no operator[]", "not Lengthable") into a
 * runtime panic.
 *
 * class_ref is the discriminator: every registry that fabricates a builtin
 * named instance leaves it NULL (xr_type_new_named_instance, the prelude
 * branches in xtype_ref_resolve.c, the .xrd handle path), while a user class
 * instance always carries the XrClassInfo it was declared from.
 *
 * Not every named-class test wants this. Stdlib classes that are themselves
 * written in xray — sys.Process, sys.Pipe — are ordinary user classes to the
 * analyzer and do carry a class_ref, so the lints over them keep using
 * xr_type_is_named_class. */
static inline bool xr_type_is_builtin_named_class(const XrType *t, const char *name) {
    return xr_type_is_named_class(t, name) && t->instance.class_ref == NULL;
}

/* Same question for a builtin that surfaces as either a native instance or as
 * the builtin interface of that name — Iterator is the one such type today.
 *
 * Both forms are decided the same way, by declaration identity. A user
 * `interface Lengthable { ... }` is as legal as `class Range { ... }`, and the
 * analyzer attaches the declaration's XrClassInfo to the interface type it
 * builds; the builtin interface registry leaves class_ref NULL. */
static inline bool xr_type_is_builtin_named_type(const XrType *t, const char *name) {
    if (!t || (t->kind != XR_KIND_INTERFACE && t->kind != XR_KIND_INSTANCE))
        return false;
    return t->instance.class_name && strcmp(t->instance.class_name, name) == 0 &&
           t->instance.class_ref == NULL;
}

/* Whether a type denotes a runtime-managed object whose lifetime belongs to
 * the runtime/scheduler, NOT the compiler's per-coroutine RC. The compiler
 * (xi_arc / xi_own) must not insert dup/drop for such values: they are held by
 * the executor past the code handle's death, so an IR-local drop could free an
 * object still in use.
 *
 * Only Task and Coroutine qualify: the scheduler owns them. Channel, Atomic,
 * WorkQueue, and ResultGroup are pure cross-coroutine shared DATA with no
 * executor owner, so they use the atomic shared-RC exactly like `shared`
 * — the compiler DOES track them (dup = atomic incref, last drop frees). Timer
 * channels keep a per-instance XR_OBJ_MANAGED backstop (the timer wheel owns
 * the embedded node asynchronously), so the runtime dup/drop primitives no-op
 * them even though the compiler emits the ops. */
static inline bool xr_type_is_runtime_managed(const XrType *t) {
    if (!t)
        return false;
    if (xr_type_is_builtin_named_class(t, "Task") || xr_type_is_builtin_named_class(t, "Coroutine"))
        return true;
    return false;
}

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
#define XR_TYPE_IS_RECORD(t) ((t)->kind == XR_KIND_RECORD)
#define XR_TYPE_HAS_OBJECT_SHAPE(t) ((t) && xr_kind_has_object_shape((t)->kind))
#define XR_TYPE_IS_TYPE_PARAM(t) ((t)->kind == XR_KIND_TYPE_PARAM)
#define XR_TYPE_IS_TUPLE(t) ((t)->kind == XR_KIND_TUPLE)
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

#define XR_UNION_MAX_MEMBERS 6

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
        case XR_KIND_RECORD:
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
        case XR_KIND_RECORD:
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
        case XR_KIND_RECORD:
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
XR_FUNC XrType *xr_type_new_record_with_fields(XrVMRuntime *X, const char **names, XrType **types,
                                               int count, XrObjectRowMode row_mode);
XR_FUNC XrType *xr_type_new_json_with_fields(XrVMRuntime *X, const char **names, XrType **types,
                                             int count, bool allows_runtime_extension);
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
XR_FUNC XrType *xr_type_new_regex(XrVMRuntime *X);
XR_FUNC XrType *xr_type_new_stringbuilder(XrVMRuntime *X);
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

static inline bool xr_type_object_row_is_exact(const XrType *type) {
    return type && XR_TYPE_IS_RECORD(type) && type->object.row_mode == XR_OBJECT_ROW_EXACT;
}

static inline bool xr_type_object_fields_are_closed(const XrType *type) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type))
        return false;
    return XR_TYPE_IS_RECORD(type) || !type->object.allows_runtime_extension;
}

static inline bool xr_type_object_accepts_extra_fields(const XrType *type) {
    if (!XR_TYPE_HAS_OBJECT_SHAPE(type))
        return false;
    if (XR_TYPE_IS_RECORD(type))
        return type->object.row_mode == XR_OBJECT_ROW_OPEN;
    return type->object.allows_runtime_extension;
}

// Json is a closed data-exchange value domain. External Xray heap values do
// not flow into it implicitly; use Json.encode at that boundary. Json literal
// contexts are handled by the analyzer by typing nested array/object literals
// as Json directly.
static inline bool xr_is_json_coercion(XrType *target, XrType *source) {
    if (!target || !source)
        return false;

    if (target->kind == XR_KIND_JSON && target->object.allows_runtime_extension) {
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
    if (target->kind == XR_KIND_JSON && target->object.allows_runtime_extension)
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
// External Record/Array/Map/Set/class values must cross through Json.encode.
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
 * Record/object-shape field-set difference, possibly nested inside an invariant
 * container. Writes a reason such as "extra field 'z'", "missing field 'y'", or
 * "element: extra field 'z'" into `buf` and returns true. Returns false for
 * scalar or otherwise unstructured mismatches, so callers keep their plain
 * "not assignable" message. Only call after xr_type_assignable() has failed. */
XR_FUNC bool xr_type_record_mismatch_reason(XrType *target, XrType *source, char *buf, size_t n);
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
 * Record descriptors and Array<Json> containers. */
XR_FUNC uint8_t xr_type_json_value_kind(const XrType *type);
XR_FUNC uint64_t xr_type_stable_key(const XrType *type);
XR_FUNC bool xr_type_is_json_decode_field_supported(const XrType *type);

#endif  // XTYPE_H
