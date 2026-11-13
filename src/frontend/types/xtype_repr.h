/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtype_repr.h - Static type representation and kind queries
 *
 * KEY CONCEPT:
 *   Each XrType has exactly one XrTypeKind.
 *   Supports union types (int | string) and T? (nullable) via is_nullable flag.
 *   Category checks (numeric, primitive, etc.) use inline functions.
 */

#ifndef XTYPE_REPR_H
#define XTYPE_REPR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xvalue.h"
#include "xslot_type.h"
#include "xstruct_layout.h"
#include "xenum_layout.h"
#include "../../base/xdefs.h"
#include "../../base/xstable_id.h"
#include "../../shared/xr_param_mode.h"
#include "../../shared/xr_view_origin.h"
#include "../../shared/xr_json_type.h"
#include "../../shared/xr_exact_scalar_registry.h"
#include "../../shared/xr_conversion.h"

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
    XR_KIND_UNION,          // Union type: int | string (compile-time only)
    XR_KIND_FIXED_ARRAY,    // Fixed-length array: [T; N] (compile-time length)
    XR_KIND_POINTER,        // FFI raw pointer: Ptr<T> (const) / MutPtr<T> (mut). Address-
                            // width integer at the value level, invisible to the GC.
    XR_KIND_RUNE,           // Unicode scalar value. Immediate value (tag XR_TAG_RUNE), not
                            // a uint32; appended last to keep existing kind values stable.
    XR_KIND_STRUCT_OBJECT,  // Exact/open structural object; shares ObjectShape metadata with Json.
    XR_KIND_SLICE,          // Borrowed contiguous view; source surface: Slice<T>.
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
/* Does a value of this kind carry a reference count at runtime?
 *
 * This is the "reference-capable" question, and it is NOT the same as move
 * semantics: `string` is a source-level value type with no movable root, yet
 * every string is a heap object the compiler must retain and release. Anything
 * deciding ownership -- return-ownership publication, ARC insertion -- asks
 * this one; only move/storage analysis asks about movable roots.
 *
 * Kind alone is not the whole answer for a concrete XrType: runtime-managed
 * objects and bare C function pointers are excluded by xi_own_type_is_rc,
 * which layers those checks on top of this. Keep the two in agreement. */
static inline bool xr_kind_is_reference_counted(XrTypeKind k) {
    switch (k) {
        case XR_KIND_INT:
        case XR_KIND_FLOAT:
        case XR_KIND_BOOL:
        case XR_KIND_RUNE:
        case XR_KIND_NULL:
        case XR_KIND_UNIT:
        case XR_KIND_NEVER:
        case XR_KIND_POINTER:
        case XR_KIND_TYPE_PARAM: /* erased; concrete rep decided after mono */
            return false;
        default:
            /* string, array, map, set, json, instance, channel, function,
             * enum, tuple, union, class, interface -> heap / RC-managed */
            return true;
    }
}
static inline bool xr_kind_has_object_shape(XrTypeKind k) {
    return k == XR_KIND_STRUCT_OBJECT;
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

typedef enum XrViewOriginBindStatus {
    XR_VIEW_ORIGIN_BIND_OK = 0,
    XR_VIEW_ORIGIN_BIND_NOT_VIEW,
    XR_VIEW_ORIGIN_BIND_MUTABLE_RETURN,
    XR_VIEW_ORIGIN_BIND_HIDDEN_RETURN,
    XR_VIEW_ORIGIN_BIND_UNKNOWN_NAME,
    XR_VIEW_ORIGIN_BIND_INELIGIBLE,
    XR_VIEW_ORIGIN_BIND_AMBIGUOUS,
    XR_VIEW_ORIGIN_BIND_INVALID,
    XR_VIEW_ORIGIN_BIND_OUT_OF_MEMORY,
} XrViewOriginBindStatus;

// Compile-time metadata for exact structural object types.
typedef struct XrObjectType {
    const char **field_names;  // Field names array
    XrType **field_types;      // Field types array (parallel to names)
    bool *field_readonly;      // Per-field readonly flags (optional)
    int field_count;           // Number of fields
    const char *type_name;     // NULL for anonymous, name for type alias
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
            XrStableId resource_id;  // Exact native declaration identity; zero for ordinary classes.
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
            XrParamMode receiver_mode;  // declaration-owned method receiver contract
            // Error-effect "may-throw" bit (task 216). Default is fail-closed
            // MAY_THROW; the analyzer proves NO_THROW after the effect-DB
            // fixpoint. POLY marks parameter-position (rethrows) function types.
            XrFnThrowEffect throw_effect;
            XrViewOrigin *view_origin_set;
            int view_origin_count;
            bool view_origin_was_elided;
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
            XrClassInfo *nominal_ref; /* declaration identity; NULL for builtins */
            uint32_t layout_id;
            const XrEnumLayout *layout;
            XrType **type_args;  // Concrete arguments retained by a generic enum type domain.
            int type_arg_count;
        } enum_type;

        // For union type (int | string) - compile-time only
        struct {
            XrType **members;      // Flat member types (sorted by kind)
            int member_count;      // Number of owned members
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

#endif /* XTYPE_REPR_H */
