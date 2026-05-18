/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass.h - Class object definition
 *
 * KEY CONCEPT:
 *   Fully static class structure, immutable after compilation.
 *   Uses Arena memory management with unified method table.
 *
 * WHY THIS DESIGN:
 *   - Static structure enables O(1) field/method lookup via Symbol
 *   - Unified method table (instance + static) with flags distinction
 */

#ifndef XCLASS_H
#define XCLASS_H

#include "../value/xvalue.h"
#include "../value/xtype.h"
#include "../../base/xhash.h"
#include "../../base/xhashmap.h"
#include "../gc/xgc_header.h"
#include <stdbool.h>

#include "xmethod.h"

typedef struct XrClass XrClass;
typedef struct XrInstance XrInstance;
typedef struct XrArena XrArena;
typedef struct XrReflectCache XrReflectCache;
typedef struct XrCoroGC XrCoroGC;
typedef struct XrCopyContext XrCopyContext;

/* ========== Native Body Descriptor ========== */

/*
 * Classes that wrap complex native state (e.g. Array buffer, Map hash
 * table, Regex NFA) attach a XrNativeBodyDesc to the XrClass. The
 * body occupies the bytes immediately after fields[] in XrInstance.
 * Classes without native state leave native_body == NULL.
 */

typedef enum XrNativeBodyCopyPolicy {
    XR_NATIVE_BODY_COPY_DEEP,    // deep_copy clones internal state
    XR_NATIVE_BODY_COPY_SHARED,  // to_shared freezes / COWs internal state
    XR_NATIVE_BODY_COPY_FORBID,  // cross-coro send / to_shared must fail
} XrNativeBodyCopyPolicy;

typedef struct XrNativeBodyDesc {
    uint32_t body_size;   // Byte size of native body (after fields[])
    uint16_t body_align;  // Alignment of body start; 0 = pointer alignment
    XrNativeBodyCopyPolicy copy_policy;
    void (*init)(XrInstance *inst, void *body);
    void (*destroy)(void *body);
    void (*traverse)(XrCoroGC *gc, void *body);
    bool (*deep_copy)(XrCopyContext *ctx, XrInstance *src, XrInstance *dst);
    bool (*to_shared)(XrayIsolate *X, XrInstance *src, XrInstance *dst);
} XrNativeBodyDesc;

/* ========== Field Access Kind ========== */

typedef enum XrFieldAccessKind {
    XR_FIELD_ACCESS_VALUE,          // Direct: inst->fields[index]
    XR_FIELD_ACCESS_NATIVE_GETTER,  // Calls a registered getter function
    XR_FIELD_ACCESS_DYNAMIC,        // Dynamic layout: in-object or overflow
} XrFieldAccessKind;

/* ========== Field Descriptor ========== */

// Field descriptor (determined at compile time, immutable)
typedef struct XrFieldDescriptor {
    const char *name;
    const char *type_name;  // Declared type name (NULL = untyped), for reflection
    int symbol;
    uint16_t offset;  // Byte offset in instance
    uint16_t flags;
    int16_t static_slot;  // Pre-computed static slot index (-1 if not static)
} XrFieldDescriptor;

// Field flags
#define XR_FIELD_PRIVATE (1 << 0)
#define XR_FIELD_PROTECTED (1 << 1)
#define XR_FIELD_STATIC (1 << 2)
#define XR_FIELD_FINAL (1 << 3)

/* ========== Method Flags ========== */

// All method flags defined in xmethod.h (XMETHOD_FLAG_*)
#include "xmethod.h"

/* ========== Dynamic Layout Transition ========== */

// Each transition records: "adding field `symbol` to class `from` yields
// class `to`". Transitions form a singly-linked list per class.
typedef struct XrClassTransition {
    int symbol;                      // Field symbol that triggers this transition
    struct XrClass *target;          // Resulting child class after adding the field
    struct XrClassTransition *next;  // Next transition in the linked list
} XrClassTransition;

/* ========== ITable Entry (opaque) ========== */

// Full layout lives in xclass_internal.h; external consumers only
// need to know XrClass carries a `XrItableEntry *itable` pointer.
typedef struct XrItableEntry XrItableEntry;

/* ========== Class Object ========== */

/*
 * XrClass - Fully static class design
 *
 * Design principles:
 * 1. All structure determined at compile time
 * 2. Completely immutable at runtime
 * 3. Descriptor arrays instead of hash tables
 * 4. O(1) direct index access
 * 5. Shareable across multiple Contexts
 */
struct XrClass {
    XrGCHeader gc;

    /* === Memory Management === */
    struct XrArena *arena;

    /* === Basic Info (immutable) === */
    const char *name;
    const char *display_name; /* User-visible name ("Box"), NULL = same as name */
    struct XrClass *super;
    struct XrClass *generic_origin; /* For mono classes: skeleton generic class; NULL otherwise */

    /* === Type Check Optimization === */
    // Primary supers array: [0]=Object, [1]=parent1, ..., [depth]=self.
    // Lets xr_class_instanceof do an O(1) array probe whenever the
    // target's depth fits in this window (depth < 8 is by far the
    // common case).
    struct XrClass *primary_supers[8];
    uint8_t depth;  // Inheritance depth (Object=0)

    // Secondary supers, populated only when depth >= 8. Open-addressing
    // hash table keyed by the ancestor XrClass* identity; capacity is a
    // power of two so the modulus is a single `& (cap - 1)`. NULL entries
    // mark empty slots (there are no deletions).
    //
    // Allocation failure is tolerated: instanceof falls back to the
    // old linear super-chain walk when secondary_supers_hash is NULL,
    // so callers never need to special-case that path.
    struct XrClass **secondary_supers_hash;
    uint16_t secondary_supers_capacity;

    /* === Field Definition === */
    XrFieldDescriptor *fields;
    XrValue *field_default_values;
    uint16_t field_count;      // Total fields (including inherited)
    uint16_t own_field_count;  // Own fields (excluding inherited)
    uint16_t instance_size;    // Instance size in bytes

    // Field lookup: Symbol -> index
    int *field_symbol_to_index;
    int field_map_capacity;

    /* === Method Table === */
    XrMethod *methods;  // All methods (instance + static)
    uint16_t method_count;
    uint16_t static_method_count;

    // Method lookup: Symbol -> index (O(1))
    int *method_symbol_to_index;
    int method_map_capacity;

    /* === VTable === */
    XrMethod **vtable;
    uint16_t vtable_size;
    uint16_t own_method_start;  // Start index of own methods in vtable

    /* === Static Fields === */
    XrValue *static_field_values;
    uint16_t static_field_count;

    /* === Interfaces === */
    struct XrClass **interfaces;
    uint8_t interface_count;

    /* === ITable === */
    XrItableEntry *itable;
    uint8_t itable_size;

    /* === Abstract Methods === */
    int *abstract_methods;  // Abstract method Symbol list
    uint8_t abstract_method_count;

    /* === Flags === */
    uint16_t flags;

    /* === Dynamic Layout (hidden class transitions) === */
    // Used only when flags & XR_CLASS_DYNAMIC_LAYOUT. Implements V8-style
    // hidden classes: adding a field creates a child class (transition).
    struct XrClassTransition *transitions;  // Linked list of transitions
    struct XrClass *transition_parent;      // Parent class in transition chain
    int transition_symbol;                  // Symbol that caused transition from parent
    uint16_t in_object_capacity;            // Max inline field slots (default 8)

    /* === Struct Layout (VALUE_TYPE only) === */
    struct XrStructLayout *struct_layout;  // NULL for class, set for struct

    /* === Operator Overload Flags === */
    uint32_t operator_flags;

    /* === Monomorphized Generics === */
    uint8_t mono_type_argc; /* 0 if not monomorphized */
    const char *
        *mono_type_arg_names; /* Concrete type display names (heap array), NULL if argc == 0 */

    /* === Native Body === */
    // Non-NULL when this class owns native C state stored after fields[]
    // in each XrInstance. Subclasses inherit (pointer copy) but cannot
    // override. Set via xr_class_builder_set_native_body().
    XrNativeBodyDesc *native_body;

    /* === Reflection Cache === */
    // Eagerly built by xr_class_builder_finalize; guaranteed non-NULL
    // for any class that survives finalize, unless its allocation hit
    // OOM. reflection clients can treat NULL as "degraded, no cache"
    // but never as "not built yet".
    struct XrReflectCache *reflect_cache;
    // Also eagerly populated by finalize via xr_registry_register_class.
    struct XrTypeMetadata *type_metadata;
};

// Class flags
#define XR_CLASS_BUILTIN (1 << 0)
#define XR_CLASS_FINAL (1 << 1)
#define XR_CLASS_ABSTRACT (1 << 2)
#define XR_CLASS_INTERFACE (1 << 3)
#define XR_CLASS_INITIALIZED (1 << 4)       // Static constructor executed
#define XR_CLASS_VALUE_TYPE (1 << 5)        // Struct: value type with copy semantics
#define XR_CLASS_FLAT_COPYABLE (1 << 6)     // All non-pointer fields are primitive (memcpy safe)
#define XR_CLASS_HAS_SUBCLASSES (1 << 7)    // At least one subclass exists (CHA devirtualization)
#define XR_CLASS_GENERIC_SKELETON (1 << 8)  // Uninstantiable generic template (e.g. Box)
#define XR_CLASS_MONOMORPHIZED (1 << 9)     // Generated by monomorphization (e.g. Box$i64)
#define XR_CLASS_DYNAMIC_LAYOUT (1 << 10)   // Dynamic field layout (Json object / object literal)
#define XR_CLASS_HAS_NATIVE_BODY (1 << 11)  // Has XrNativeBodyDesc (Array, Map, etc.)
#define XR_CLASS_DYNAMIC_SEALED (1 << 12)   // Dynamic-layout class rejects new field transitions

/* ========== Operator Overload Flags ========== */

// Fast check for operator overload, avoiding unnecessary method lookup

// Arithmetic operators
#define XR_OP_ADD_FLAG (1U << 0)  // +
#define XR_OP_SUB_FLAG (1U << 1)  // -
#define XR_OP_MUL_FLAG (1U << 2)  // *
#define XR_OP_DIV_FLAG (1U << 3)  // /
#define XR_OP_MOD_FLAG (1U << 4)  // %

// Comparison operators
#define XR_OP_EQ_FLAG (1U << 5)   // ==
#define XR_OP_NE_FLAG (1U << 6)   // !=
#define XR_OP_LT_FLAG (1U << 7)   // <
#define XR_OP_LE_FLAG (1U << 8)   // <=
#define XR_OP_GT_FLAG (1U << 9)   // >
#define XR_OP_GE_FLAG (1U << 10)  // >=

// Bitwise operators
#define XR_OP_BAND_FLAG (1U << 11)    // &
#define XR_OP_BOR_FLAG (1U << 12)     // |
#define XR_OP_BXOR_FLAG (1U << 13)    // ^
#define XR_OP_BNOT_FLAG (1U << 14)    // ~
#define XR_OP_LSHIFT_FLAG (1U << 15)  // <<
#define XR_OP_RSHIFT_FLAG (1U << 16)  // >>

// Special operators
#define XR_OP_INDEX_FLAG (1U << 17)      // []
#define XR_OP_INDEX_SET_FLAG (1U << 18)  // []=
#define XR_OP_INC_FLAG (1U << 19)        // ++
#define XR_OP_DEC_FLAG (1U << 20)        // --
#define XR_OP_NOT_FLAG (1U << 21)        // !

// Helper macros: operator category combinations
#define XR_OP_ANY_ARITHMETIC                                                                       \
    (XR_OP_ADD_FLAG | XR_OP_SUB_FLAG | XR_OP_MUL_FLAG | XR_OP_DIV_FLAG | XR_OP_MOD_FLAG)
#define XR_OP_ANY_COMPARISON                                                                       \
    (XR_OP_EQ_FLAG | XR_OP_NE_FLAG | XR_OP_LT_FLAG | XR_OP_LE_FLAG | XR_OP_GT_FLAG | XR_OP_GE_FLAG)
#define XR_OP_ANY_BITWISE                                                                          \
    (XR_OP_BAND_FLAG | XR_OP_BOR_FLAG | XR_OP_BXOR_FLAG | XR_OP_BNOT_FLAG | XR_OP_LSHIFT_FLAG |    \
     XR_OP_RSHIFT_FLAG)

// Check if class has specific operator overload
#define XCLASS_HAS_OP(cls, op_flag) ((cls) && ((cls)->operator_flags & (op_flag)))

// Check if class has any operator overload
#define XCLASS_HAS_ANY_OP(cls) ((cls) && ((cls)->operator_flags != 0))

// xr_symbol_to_op_flag and xr_class_compute_operator_flags are
// build-time helpers; their declarations live in xclass_internal.h.

/* ========== Class Builder ========== */

// The full XrClassBuilder API lives in xclass_builder.h. Callers that
// only need to talk to existing classes should not have to pull that
// header in, so xclass.h exposes nothing more than the opaque handle
// and the finalize entry point needed by a few build-time hooks.
typedef struct XrClassBuilder XrClassBuilder;

/* ========== Runtime API (read-only) ========== */

// Instance field count (total fields minus static fields)
static inline uint16_t xr_class_instance_field_count(const XrClass *cls) {
    return cls->field_count - cls->static_field_count;
}

// Returns NULL for invalid index
XR_FUNC const XrFieldDescriptor *xr_class_get_field(const XrClass *cls, int index);

// Lookup field index by name, returns -1 if not found.
// Name is resolved via the isolate's symbol table, so the lookup is
// O(1) whenever the name has been interned. `X` is required.
XR_FUNC int xr_class_lookup_field_by_name(XrayIsolate *X, XrClass *cls, const char *name);

// Lookup field index by symbol, returns -1 if not found
XR_FUNC int xr_class_lookup_field(XrClass *cls, int symbol);

static inline const char *xr_class_get_name(XrClass *cls) {
    return cls ? cls->name : NULL;
}

/* User-visible class name: returns display_name when set (strips
 * monomorphization suffix), otherwise falls back to internal name. */
static inline const char *xr_class_display_name(const XrClass *cls) {
    if (!cls)
        return "<null>";
    return cls->display_name ? cls->display_name : cls->name;
}

// Lookup method by symbol, returns NULL if not found
XR_FUNC XrMethod *xr_class_lookup_method(XrClass *cls, int symbol);

// NOTE: No add/set/modify API! All modifications via XrClassBuilder.

/* ========== Direct Class API ========== */

// Create a bare class (no fields, no methods, no interfaces). Internally
// this is a thin wrapper over XrClassBuilder; the returned object is
// already finalised and immutable. Use the builder directly when members
// need to be installed.
XR_FUNC XrClass *xr_class_new(XrayIsolate *X, const char *name, XrClass *super);
XR_FUNC void xr_class_mark_abstract(XrClass *cls);

// Get class for any value (instance, class, or primitive)
XR_FUNC XrClass *xr_value_get_class(XrayIsolate *X, XrValue value);

/* ========== Access Control ========== */

static inline bool xr_class_is_field_private(const XrClass *cls, int index) {
    if (index >= 0 && index < cls->field_count) {
        return (cls->fields[index].flags & XR_FIELD_PRIVATE) != 0;
    }
    return false;
}

// xr_class_is_method_private / xr_class_has_method_by_symbol were never
// reached from anywhere outside this header; their work is covered by
// xr_class_lookup_method + a flags check on the returned XrMethod.

/* ========== Helper Functions ========== */

// Check if cls is subclass of target (or same class).
//
// Three tiers, each O(1) on its own inputs:
//   1. cls == target  -- identity
//   2. target->depth < 8
//        -> cls->primary_supers[target->depth] == target
//      This covers the overwhelmingly common case (the xray stdlib
//      and every builtin type live here).
//   3. target->depth >= 8
//        -> probe cls->secondary_supers_hash (open addressing,
//           capacity is power-of-two so the modulus is a mask).
//      Falls back to a linear xs super-chain walk if the hash could
//      not be allocated at finalize time.
static inline bool xr_class_instanceof(const XrClass *cls, const XrClass *target) {
    if (cls == NULL || target == NULL) {
        return false;
    }

    if (cls == target) {
        return true;
    }

    /* Monomorphized class matches its generic origin:
     * e.g. Box$i64 is Box → true when target is the skeleton class. */
    if (cls->generic_origin != NULL && cls->generic_origin == target) {
        return true;
    }

    if (target->depth < 8) {
        return cls->primary_supers[target->depth] == target;
    }

    if (cls->secondary_supers_hash != NULL && cls->secondary_supers_capacity > 0) {
        uint32_t mask = (uint32_t) cls->secondary_supers_capacity - 1;
        uint32_t h = xr_hash_int((int64_t) (uintptr_t) target) & mask;
        for (;;) {
            struct XrClass *slot = cls->secondary_supers_hash[h];
            if (slot == target)
                return true;
            if (slot == NULL)
                return false;
            h = (h + 1) & mask;
        }
    }

    // Fallback: hash table was not built (allocation failure path).
    const XrClass *c = cls->super;
    while (c != NULL) {
        if (c == target) {
            return true;
        }
        c = c->super;
    }

    return false;
}

XR_FUNC bool xr_instance_of(void *obj, const XrClass *target);

// Debug: print class info
XR_FUNC void xr_class_print(XrClass *cls);

/* ========== Interface Support ========== */

XR_FUNC XrClass *xr_interface_new(XrayIsolate *X, const char *name);

// Pointer-identity interface check that walks cls's inheritance chain
// and scans cls->interfaces[] for `iface`. No string compares -- every
// interface slot is an XrClass* so the check is an O(depth*width) walk
// of pointers. `iface` must have the XR_CLASS_INTERFACE flag set.
XR_FUNC bool xr_class_implements_interface(XrClass *cls, XrClass *iface);

/* ========== Abstract Class Support ========== */

XR_FUNC bool xr_class_can_instantiate(XrClass *cls);

// NOTE: static and instance methods share the same methods[] array.
// Discriminate via method->flags & XMETHOD_FLAG_STATIC. There is
// deliberately no dedicated "static method count" accessor; the
// static count is already a struct field on XrClass.

#endif  // XCLASS_H
