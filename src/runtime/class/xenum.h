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
 *   Runtime enum values are safe tag+payload aggregates. Payload-bearing
 *   variants use a small constructor metadata object so `Result.Ok(42)` can
 *   remain a normal call without exposing a runtime enum wrapper class.
 */

#ifndef XENUM_H
#define XENUM_H

#include "../value/xvalue.h"
#include "../value/xenum_layout.h"
#include "../../shared/xr_derive_flags.h"
#include <stdint.h>

struct XrRuntimeCore;

// Payload variant constructor metadata (e.g. Result.Ok before it is called).
// GC tag is XR_TENUM_CTOR; it is not a user-visible class instance.
typedef struct XrEnumCtor {
    XrObjHeader hdr;
    // Both names are interned in the isolate's symbol table; not owned.
    const char *enum_name;
    const char *member_name;
    uint32_t member_index;
    uint32_t layout_id;
    struct XrEnumType *parent_type;  // Back-pointer for ADT variant construction
} XrEnumCtor;

/* Runtime ADT enum value. It shares only the object header + klass prefix with
 * XrInstance; tag and payload are enum-owned inline storage, not generic
 * instance fields. */
typedef struct XrEnumAggregateValue {
    XrObjHeader hdr;
    struct XrClass *klass;  // enum_type->enum_class, builtin_kind XR_BK_ADT_ENUM
    struct XrEnumType *enum_type;
    uint32_t member_index;
    uint32_t payload_count;
    XrValue payloads[];
} XrEnumAggregateValue;

// Enum type/namespace metadata (immutable at runtime).
// GC tag is XR_TENUM_TYPE; it is not a user-visible wrapper class.
typedef struct XrEnumType {
    XrObjHeader hdr;
    const char *name;  // Interned in symbol table; not owned.
    uint32_t member_count;
    struct XrClass *enum_class;

    struct XrEnumMember {
        const char *name;  // Interned in symbol table; not owned.
        int symbol;
        XrEnumCtor *ctor;
        XrEnumAggregateValue *value;  // Canonical 0-payload aggregate, lazily allocated.
    } *members;

    int *symbol_to_index;  // symbol -> member index
    int symbol_map_capacity;
    XrEnumLayout *layout;
    uint32_t derive_flags;

    /* Core-only AOT/runtime enums create a minimal field-layout class without
     * VM type registration. VM-created enums leave this false because
     * their enum_class is owned by the class system. */
    bool owns_enum_class;
} XrEnumType;

/* ========== Creation ========== */

XR_FUNC XrEnumType *xr_enum_type_new(XrVMRuntime *X, const char *nominal_owner, const char *name,
                                     char **member_names, int count);
XR_FUNC XrEnumType *xr_enum_type_new_core(struct XrRuntimeCore *core,
                                          const char *nominal_owner, const char *name,
                                          char **member_names, int count);

XR_FUNC XrEnumCtor *xr_enum_ctor_new(XrVMRuntime *X, const char *enum_name, const char *member_name,
                                     uint32_t index);
XR_FUNC XrEnumCtor *xr_enum_ctor_new_core(struct XrRuntimeCore *core, const char *enum_name,
                                          const char *member_name, uint32_t index);
XR_FUNC bool xr_enum_type_set_adt_payloads(XrEnumType *enum_type, const int *payload_counts,
                                           int count);
XR_FUNC bool xr_enum_type_ensure_adt_class(XrEnumType *enum_type);
XR_FUNC bool xr_enum_type_has_payloads(const XrEnumType *enum_type);
XR_FUNC int xr_enum_type_payload_count(const XrEnumType *enum_type, uint32_t member_index);
XR_FUNC uint16_t xr_enum_type_max_payload(const XrEnumType *enum_type);
XR_FUNC const char *xr_enum_type_member_name(const XrEnumType *enum_type, uint32_t member_index);

/* ========== Access ========== */

XR_FUNC XrEnumCtor *xr_enum_get_member_by_symbol(XrEnumType *enum_type, int symbol);
XR_FUNC int xr_enum_type_find_member_index_by_symbol(XrEnumType *enum_type, int symbol);
XR_FUNC const char *xr_enum_ctor_name(XrEnumCtor *ctor);
XR_FUNC XrEnumAggregateValue *
xr_enum_zero_payload_value(struct XrVMRuntime *X, XrEnumType *enum_type, uint32_t member_index);

/* ========== Enum Aggregate Construction ========== */

/* Construct an enum aggregate value. Logical field 0 is the tag; logical
 * fields 1..N are payloads. Runtime storage is XrEnumAggregateValue, not
 * generic XrInstance fields. */
struct XrAllocationContext;
XR_FUNC size_t xr_enum_aggregate_size(uint32_t payload_count);
XR_FUNC void xr_enum_aggregate_init_inplace(XrEnumAggregateValue *value, XrEnumType *enum_type,
                                            uint32_t member_index, const XrValue *payloads,
                                            uint32_t payload_count);
XR_FUNC struct XrEnumAggregateValue *xr_enum_adt_construct_in(struct XrAllocationContext *alloc,
                                                              XrEnumType *enum_type,
                                                              uint32_t member_index, XrValue *args,
                                                              int nargs);
XR_FUNC struct XrEnumAggregateValue *xr_enum_adt_construct(struct XrVMRuntime *X,
                                                           XrEnumType *enum_type,
                                                           uint32_t member_index, XrValue *args,
                                                           int nargs);
XR_FUNC struct XrEnumAggregateValue *
xr_enum_adt_construct_storage(struct XrVMRuntime *X, XrEnumType *enum_type, uint32_t member_index,
                              XrValue *args, int nargs, uint8_t storage_mode);
XR_FUNC bool xr_value_is_enum_aggregate(XrValue value);
XR_FUNC XrEnumAggregateValue *xr_value_to_enum_aggregate(XrValue value);
XR_FUNC XrEnumType *xr_enum_aggregate_type(const XrEnumAggregateValue *value);
XR_FUNC const char *xr_enum_aggregate_member_name(const XrEnumAggregateValue *value);
XR_FUNC XrValue xr_enum_aggregate_field_get(const XrEnumAggregateValue *value, int64_t index);
XR_FUNC XrValue xr_enum_aggregate_payload_get(const XrEnumAggregateValue *value, uint32_t index);

/* ========== Symbol Mapping ========== */

XR_FUNC void xr_enum_type_init_symbols(XrEnumType *enum_type, void *isolate);

/* ========== Destroy Hooks ==========
 * Both objects live on the isolate fixed_heap list. The hooks below are
 * registered in the per-type destroy table so xr_fixed_heap_cleanup releases their
 * malloc-backed side resources before freeing the body. Callers never
 * free enum objects manually. */

struct XrObjHeader;
struct XrCoroHeap;

XR_FUNC void xr_obj_destroy_enum_type(struct XrObjHeader *obj, struct XrCoroHeap *owner_heap);
XR_FUNC void xr_obj_destroy_enum_ctor(struct XrObjHeader *obj, struct XrCoroHeap *owner_heap);

/* ========== Type Conversion ========== */

#define XR_TO_ENUM_TYPE(v) ((XrEnumType *) XR_TO_PTR(v))
#define XR_TO_ENUM_CTOR(v) ((XrEnumCtor *) XR_TO_PTR(v))

#endif  // XENUM_H
