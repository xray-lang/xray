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
#include "../../base/xhashmap.h"
#include "../gc/xgc_header.h"
#include <stdbool.h>

#include "xmethod.h"

typedef struct XrClass XrClass;
typedef struct XrInstance XrInstance;
typedef struct XrArena XrArena;
typedef struct XrReflectCache XrReflectCache;

/* ========== Field Descriptor ========== */

// Field descriptor (determined at compile time, immutable)
typedef struct XrFieldDescriptor {
    const char *name;
    const char *type_name;   // Declared type name (NULL = untyped), for reflection
    int symbol;
    uint16_t offset;         // Byte offset in instance
    uint16_t flags;
    int16_t static_slot;     // Pre-computed static slot index (-1 if not static)
} XrFieldDescriptor;

// Field flags
#define XR_FIELD_PRIVATE    (1 << 0)
#define XR_FIELD_PROTECTED  (1 << 1)
#define XR_FIELD_STATIC     (1 << 2)
#define XR_FIELD_FINAL      (1 << 3)

/* ========== Method Flags ========== */

// All method flags defined in xmethod.h (XMETHOD_FLAG_*)
#include "xmethod.h"

/* ========== ITable Entry ========== */

// Interface table entry for interface method dispatch
typedef struct XrItableEntry {
    struct XrClass *interface;
    XrMethod **methods;
    uint16_t method_count;
} XrItableEntry;

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
    struct XrClass *super;
    
    /* === Type Check Optimization === */
    // Primary supers array: [0]=Object, [1]=parent1, ..., [depth]=self
    struct XrClass *primary_supers[8];
    uint8_t depth;                     // Inheritance depth (Object=0, max 7)
    
    /* === Field Definition === */
    XrFieldDescriptor *fields;
    XrValue *field_default_values;
    uint16_t field_count;              // Total fields (including inherited)
    uint16_t own_field_count;          // Own fields (excluding inherited)
    uint16_t instance_size;            // Instance size in bytes
    
    // Field lookup: Symbol -> index
    int *field_symbol_to_index;
    int field_map_capacity;
    
    /* === Method Table === */
    XrMethod *methods;                 // All methods (instance + static)
    uint16_t method_count;
    uint16_t static_method_count;
    
    // Method lookup: Symbol -> index (O(1))
    int *method_symbol_to_index;
    int method_map_capacity;
    
    /* === VTable === */
    XrMethod **vtable;
    uint16_t vtable_size;
    uint16_t own_method_start;         // Start index of own methods in vtable
    
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
    int *abstract_methods;             // Abstract method Symbol list
    uint8_t abstract_method_count;
    
    /* === Flags === */
    uint16_t flags;
    
    /* === Struct Layout (VALUE_TYPE only) === */
    struct XrStructLayout *struct_layout;  // NULL for class, set for struct
    
    /* === Operator Overload Flags === */
    uint32_t operator_flags;
    
    /* === Reflection Cache === */
    struct XrReflectCache *reflect_cache;  // Per-class, lazy creation
    struct XrTypeMetadata *type_metadata;  // Cached, set on first registry lookup
};

// Class flags
#define XR_CLASS_BUILTIN      (1 << 0)
#define XR_CLASS_FINAL        (1 << 1)
#define XR_CLASS_ABSTRACT     (1 << 2)
#define XR_CLASS_INTERFACE    (1 << 3)
#define XR_CLASS_INITIALIZED  (1 << 4)  // Static constructor executed
#define XR_CLASS_VALUE_TYPE   (1 << 5)  // Struct: value type with copy semantics
#define XR_CLASS_FLAT_COPYABLE (1 << 6) // All non-pointer fields are primitive (memcpy safe)
#define XR_CLASS_HAS_SUBCLASSES (1 << 7) // At least one subclass exists (CHA devirtualization)

/* ========== Operator Overload Flags ========== */

// Fast check for operator overload, avoiding unnecessary method lookup

// Arithmetic operators
#define XR_OP_ADD_FLAG      (1U << 0)   // +
#define XR_OP_SUB_FLAG      (1U << 1)   // -
#define XR_OP_MUL_FLAG      (1U << 2)   // *
#define XR_OP_DIV_FLAG      (1U << 3)   // /
#define XR_OP_MOD_FLAG      (1U << 4)   // %

// Comparison operators
#define XR_OP_EQ_FLAG       (1U << 5)   // ==
#define XR_OP_NE_FLAG       (1U << 6)   // !=
#define XR_OP_LT_FLAG       (1U << 7)   // <
#define XR_OP_LE_FLAG       (1U << 8)   // <=
#define XR_OP_GT_FLAG       (1U << 9)   // >
#define XR_OP_GE_FLAG       (1U << 10)  // >=

// Bitwise operators
#define XR_OP_BAND_FLAG     (1U << 11)  // &
#define XR_OP_BOR_FLAG      (1U << 12)  // |
#define XR_OP_BXOR_FLAG     (1U << 13)  // ^
#define XR_OP_BNOT_FLAG     (1U << 14)  // ~
#define XR_OP_LSHIFT_FLAG   (1U << 15)  // <<
#define XR_OP_RSHIFT_FLAG   (1U << 16)  // >>

// Special operators
#define XR_OP_INDEX_FLAG     (1U << 17)  // []
#define XR_OP_INDEX_SET_FLAG (1U << 18)  // []= 
#define XR_OP_INC_FLAG       (1U << 19)  // ++
#define XR_OP_DEC_FLAG       (1U << 20)  // --
#define XR_OP_NOT_FLAG       (1U << 21)  // !

// Helper macros: operator category combinations
#define XR_OP_ANY_ARITHMETIC (XR_OP_ADD_FLAG | XR_OP_SUB_FLAG | XR_OP_MUL_FLAG | XR_OP_DIV_FLAG | XR_OP_MOD_FLAG)
#define XR_OP_ANY_COMPARISON (XR_OP_EQ_FLAG | XR_OP_NE_FLAG | XR_OP_LT_FLAG | XR_OP_LE_FLAG | XR_OP_GT_FLAG | XR_OP_GE_FLAG)
#define XR_OP_ANY_BITWISE    (XR_OP_BAND_FLAG | XR_OP_BOR_FLAG | XR_OP_BXOR_FLAG | XR_OP_BNOT_FLAG | XR_OP_LSHIFT_FLAG | XR_OP_RSHIFT_FLAG)

// Check if class has specific operator overload
#define XCLASS_HAS_OP(cls, op_flag) \
    ((cls) && ((cls)->operator_flags & (op_flag)))

// Check if class has any operator overload
#define XCLASS_HAS_ANY_OP(cls) \
    ((cls) && ((cls)->operator_flags != 0))

// Get flag for operator symbol, returns 0 if unknown
XR_FUNC uint32_t xr_symbol_to_op_flag(int symbol);

// Compute operator overload flags for class (call once after class creation)
XR_FUNC void xr_class_compute_operator_flags(XrClass *cls);

/* ========== Class Builder API (compile time) ========== */

/*
 * Class builder - Used only at compile/load time
 *
 * Lifecycle:
 *   1. xr_class_builder_new()      - Create builder
 *   2. xr_class_builder_add_xxx()  - Add fields/methods/interfaces
 *   3. xr_class_builder_finalize() - Freeze into immutable class
 *
 * NOTE: Builder is destroyed after finalize, returned class is immutable
 */
typedef struct XrClassBuilder XrClassBuilder;

XR_FUNC XrClassBuilder* xr_class_builder_new(XrayIsolate *isolate, 
                                      const char *name,
                                      XrClass *super);

XR_FUNC int xr_class_builder_add_field(XrClassBuilder *builder,
                                const char *name,
                                uint32_t flags);

XR_FUNC int xr_class_builder_add_method(XrClassBuilder *builder,
                                 const char *name,
                                 XrCFunctionPtr impl,
                                 int param_count,
                                 uint32_t flags);

XR_FUNC int xr_class_builder_add_static_field(XrClassBuilder *builder,
                                       const char *name,
                                       XrValue value,
                                       uint32_t flags);

XR_FUNC int xr_class_builder_add_interface(XrClassBuilder *builder,
                                    XrClass *interface);

XR_FUNC int xr_class_builder_add_abstract_method(XrClassBuilder *builder,
                                          int method_symbol);

// Finalize builder and return immutable class (builder is destroyed)
XR_FUNC XrClass* xr_class_builder_finalize(XrClassBuilder *builder);

/* ========== Runtime API (read-only) ========== */

// Instance field count (total fields minus static fields)
static inline uint16_t xr_class_instance_field_count(const XrClass *cls) {
    return cls->field_count - cls->static_field_count;
}

// Returns NULL for invalid index
XR_FUNC const XrFieldDescriptor* xr_class_get_field(const XrClass *cls, int index);

// Lookup field index by name, returns -1 if not found
XR_FUNC int xr_class_lookup_field_by_name(XrClass *cls, const char *name);

// Lookup field index by symbol, returns -1 if not found
XR_FUNC int xr_class_lookup_field(XrClass *cls, int symbol);

static inline const char* xr_class_get_name(XrClass *cls) {
    return cls ? cls->name : NULL;
}

// Lookup method by symbol, returns NULL if not found
XR_FUNC XrMethod* xr_class_lookup_method(XrClass *cls, int symbol);

// Get method C function pointer, returns NULL if not found
XR_FUNC XrCFunctionPtr xr_class_get_method(const XrClass *cls, int symbol);

XR_FUNC XrValue xr_class_get_static_field(const XrClass *cls, int index);

// NOTE: No add/set/modify API! All modifications via XrClassBuilder.

/* ========== Direct Class API ========== */

// Create class directly (used by builtin class initialization)
XR_FUNC XrClass* xr_class_new(XrayIsolate *X, const char *name, XrClass *super);
XR_FUNC void xr_class_mark_abstract(XrClass *cls);
XR_FUNC void xr_class_add_abstract_method(XrClass *cls, int method_symbol);
XR_FUNC void xr_class_set_super(XrClass *subclass, XrClass *superclass);

XR_FUNC void xr_class_free(XrClass *cls);

// Get class for any value (instance, class, or primitive)
XR_FUNC XrClass* xr_value_get_class(XrayIsolate *X, XrValue value);

/* ========== Access Control ========== */

static inline bool xr_class_is_field_private(const XrClass *cls, int index) {
    if (index >= 0 && index < cls->field_count) {
        return (cls->fields[index].flags & XR_FIELD_PRIVATE) != 0;
    }
    return false;
}

// Check if method is private
static inline bool xr_class_is_method_private(const XrClass *cls, int symbol) {
    if (!cls || !cls->method_symbol_to_index || symbol < 0 || symbol >= cls->method_map_capacity) {
        return false;
    }
    int idx = cls->method_symbol_to_index[symbol];
    if (idx < 0 || idx >= cls->method_count) {
        return false;
    }
    if (cls->methods[idx].type == XMETHOD_NONE) {
        return false;
    }
    return (cls->methods[idx].flags & XMETHOD_FLAG_PRIVATE) != 0;
}

// Check if class has method (ignoring access control)
static inline bool xr_class_has_method_by_symbol(const XrClass *cls, int symbol) {
    if (!cls || !cls->method_symbol_to_index || symbol < 0 || symbol >= cls->method_map_capacity) {
        return false;
    }
    int idx = cls->method_symbol_to_index[symbol];
    if (idx < 0 || idx >= cls->method_count) {
        return false;
    }
    return cls->methods[idx].type != XMETHOD_NONE;
}

/* ========== Helper Functions ========== */

// Check if cls is subclass of target (or same class)
// O(1) for depth < 8 using primary_supers array
static inline bool xr_class_instanceof(const XrClass *cls, const XrClass *target) {
    if (cls == NULL || target == NULL) {
        return false;
    }
    
    if (cls == target) {
        return true;
    }
    
    // O(1) lookup via primary_supers
    if (target->depth < 8) {
        return cls->primary_supers[target->depth] == target;
    }
    
    // Fallback to linear search for depth >= 8 (rare)
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

XR_FUNC XrClass* xr_interface_new(XrayIsolate *X, const char *name);

XR_FUNC bool xr_class_implements_interface(XrClass *cls, const char *interface_name);
XR_FUNC bool xr_class_implements_interface_fast(XrClass *cls, XrClass *iface);

// Returns count of satisfied methods (equals interface method count if fully satisfied)
XR_FUNC int xr_class_verify_interface(XrClass *cls, XrClass *iface, 
                               char **errors, int max_errors);

XR_FUNC bool xr_class_has_method(XrClass *cls, int method_symbol);

/* ========== ITable Generation & Lookup ========== */

// Build ITable from class's interfaces array
XR_FUNC int xr_class_build_itable(XrClass *cls);

// O(n) where n = interface count
XR_FUNC XrMethod* xr_class_lookup_interface_method(XrClass *cls, XrClass *iface, int method_index);
XR_FUNC XrMethod* xr_class_lookup_interface_method_by_symbol(XrClass *cls, XrClass *iface, int method_symbol);

/* ========== Abstract Class Support ========== */

XR_FUNC bool xr_class_can_instantiate(XrClass *cls);
XR_FUNC void xr_class_inherit_abstract_methods(XrClass *child, XrClass *parent);
XR_FUNC bool xr_class_is_abstract_method(XrClass *cls, int method_symbol);

/* ========== Static Method Access ========== */

// Get static method count
static inline uint16_t xr_class_get_static_method_count(XrClass *cls) {
    return cls ? cls->static_method_count : 0;
}

// Static and instance methods stored together
// Use xr_class_lookup_method() for all methods
// Check method->flags & XMETHOD_FLAG_STATIC for static methods

#endif // XCLASS_H
