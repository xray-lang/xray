/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xenum.h - Enum object definition
 *
 * KEY CONCEPT:
 *   Lightweight singleton enum values with fast pointer comparison.
 *   Enum type metadata created at compile time, read-only at runtime.
 */

#ifndef XENUM_H
#define XENUM_H

#include "../value/xvalue.h"
#include <stdint.h>

// Singleton enum member (e.g., Status.Success)
// Layout-compatible with XrInstance + native body (0 fields).
// GC tag is XR_TINSTANCE; class carries XR_CLASS_ENUM_VALUE flag.
typedef struct XrEnumValue {
    XrGCHeader gc;
    struct XrClass *klass;  // Points to enumValueClass
    // Both names are interned in the isolate's symbol table; not owned.
    const char *enum_name;
    const char *member_name;
    XrValue raw_value;
    uint32_t member_index;
} XrEnumValue;

// Enum type metadata (immutable at runtime)
// Layout-compatible with XrInstance + native body (0 fields).
// GC tag is XR_TINSTANCE; class carries XR_CLASS_ENUM_TYPE flag.
typedef struct XrEnumType {
    XrGCHeader gc;
    struct XrClass *klass;  // Points to enumTypeClass
    const char *name;       // Interned in symbol table; not owned.
    int base_type;
    uint32_t member_count;
    struct XrClass *enum_class;

    struct XrEnumMember {
        const char *name;  // Interned in symbol table; not owned.
        int symbol;
        XrValue value;
        XrEnumValue *instance;
    } *members;

    int *symbol_to_index;  // symbol -> member index
    int symbol_map_capacity;

    /* Reverse lookup optimization (Lua table array-part inspired)
     * Tier 1: contiguous int (0,1,2... or N,N+1,...) → direct index
     * Tier 2: sparse int with bounded range → sparse array
     * Tier 3: fallback → linear scan */
    bool is_contiguous_int;  // tier 1: values are min,min+1,...,min+N-1
    int64_t min_int_value;   // tier 1: offset for direct index
    int *value_to_index;     // tier 2: sparse array [val - min_int_value] → member index
    int value_map_range;     // tier 2: array size (max - min + 1), 0 if not used

    /* ADT enum metadata.  When is_adt is true, members carry variant tags
     * (0..N-1) instead of user-defined backing values.  Each variant may
     * have payload fields; payload_counts[i] gives the number of fields
     * for member i.  max_payload is the largest count (determines instance
     * field layout: field[0]=tag, field[1..max_payload]=payload). */
    bool is_adt;
    int max_payload;      // max(payload_counts[0..member_count-1])
    int *payload_counts;  // per-variant payload field count; NULL for simple enums
} XrEnumType;

/* ========== Creation ========== */

XR_FUNC XrEnumType *xr_enum_type_new(XrayIsolate *X, const char *name, int base_type,
                                     char **member_names, XrValue *member_values, int count);

XR_FUNC XrEnumValue *xr_enum_value_new(XrayIsolate *X, const char *enum_name,
                                       const char *member_name, XrValue raw_value, uint32_t index);

/* ========== Access ========== */

XR_FUNC XrEnumValue *xr_enum_get_member_by_symbol(XrEnumType *enum_type, int symbol);
XR_FUNC XrEnumValue *xr_enum_from_value(XrEnumType *enum_type, XrValue value);
XR_FUNC const char *xr_enum_value_name(XrEnumValue *enum_val);

/* ========== Symbol Mapping ========== */

XR_FUNC void xr_enum_type_init_symbols(XrEnumType *enum_type, void *isolate);

/* ========== Destroy Hooks ==========
 * Both objects live on the isolate fixedgc list. The hooks below are
 * registered in g_type_ops[] so xr_gc_cleanup releases their
 * malloc-backed side resources before freeing the body. Callers never
 * free enum objects manually. */

struct XrGCHeader;
struct XrCoroGC;

XR_FUNC void xr_gc_destroy_enum_type(struct XrGCHeader *obj, struct XrCoroGC *owning_gc);
XR_FUNC void xr_gc_destroy_enum_value(struct XrGCHeader *obj, struct XrCoroGC *owning_gc);

/* ========== Type Conversion ========== */

#define XR_TO_ENUM_TYPE(v) ((XrEnumType *) XR_TO_PTR(v))
#define XR_TO_ENUM_VALUE(v) ((XrEnumValue *) XR_TO_PTR(v))

/* ========== Native Body Descriptors ========== */

struct XrNativeBodyDesc;
XR_FUNC struct XrNativeBodyDesc *xr_enum_value_native_body_desc(void);
XR_FUNC struct XrNativeBodyDesc *xr_enum_type_native_body_desc(void);

#endif  // XENUM_H
