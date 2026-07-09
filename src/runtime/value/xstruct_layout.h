/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstruct_layout.h - Compile-time aggregate layout descriptor
 *
 * KEY CONCEPT:
 *   Each fixed-layout aggregate has a layout computed at compile time.
 *   Fields are stored in native format (double/int64_t/bool) without
 *   XrValue boxing. The layout drives stack-frame struct_area allocation
 *   and OP_AGG_GET/SET field access.
 *
 * MEMORY LAYOUT (example Vec3{x:float, y:float, z:float}):
 *
 *   struct_area + offset
 *   +----------+----------+----------+
 *   | x: f64   | y: f64   | z: f64   |
 *   | 8 bytes  | 8 bytes  | 8 bytes  |
 *   +----------+----------+----------+
 *   offset: 0        8        16
 *   total_size = 24, alignment = 8
 *
 * WHY THIS DESIGN:
 *   - Zero GC overhead: non-escaping aggregates live on the stack frame
 *   - Native field access: base_ptr + compile-time offset
 *   - Minimal copy cost: memcpy(total_size) for value semantics
 *   - AOT-friendly: direct C struct/union mapping
 */

#ifndef XSTRUCT_LAYOUT_H
#define XSTRUCT_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>
#include "../../base/xdefs.h"
#include "../../shared/xr_native_type_core.h"

struct XrAggregateLayout;
typedef struct XrType XrType;

typedef enum {
    XR_AGG_LAYOUT_STRUCT = 0,
    XR_AGG_LAYOUT_PACKED_STRUCT = 1,
    XR_AGG_LAYOUT_UNION = 2,
} XrAggregateLayoutKind;

// Per-field descriptor within an aggregate layout.
typedef struct {
    uint16_t offset;                       // byte offset within struct
    uint8_t native_type;                   // XrNativeType
    uint16_t size;                         // field size in bytes
    uint16_t sub_layout_id;                // layout_id for nested aggregate
    struct XrAggregateLayout *sub_layout;  // nested struct/union layout
    uint8_t elem_native_type;              // element type for XR_NATIVE_ARRAY
    uint16_t elem_count;                   // element count for XR_NATIVE_ARRAY
} XrAggregateFieldLayout;

#define XR_MAX_AGG_FIELDS 64

/*
 * XrAggregateLayout - Fixed layout for a struct, packed struct, or union type.
 *
 * Allocated once at compile time, shared by all instances of the type.
 * Referenced by nominal value types that carry fixed aggregate storage.
 */
typedef struct XrAggregateLayout {
    uint16_t total_size;       // total aggregate size in bytes (aligned)
    uint32_t alignment;        // alignment requirement
    uint16_t field_count;      // number of fields
    uint16_t layout_id;        // global layout registry index
    uint8_t kind;              // XrAggregateLayoutKind
    uint32_t explicit_align;   // align(N), 0 = natural
    const char **field_names;  // [field_count] parallel to fields[], NULL-able
    XrAggregateFieldLayout fields[XR_MAX_AGG_FIELDS];
} XrAggregateLayout;

static inline bool xr_aggregate_layout_is_headerless(const XrAggregateLayout *layout) {
    return layout != NULL;
}

static inline uint16_t xr_aggregate_layout_header_size(const XrAggregateLayout *layout) {
    (void) layout;
    return 0;
}

static inline uint32_t xr_aggregate_layout_storage_size(const XrAggregateLayout *layout) {
    if (!layout)
        return 0;
    return (uint32_t) xr_aggregate_layout_header_size(layout) + (uint32_t) layout->total_size;
}

/* ========== Layout Computation ========== */

/* Initialize a layout and compute field offsets + total size.
 * Caller must have set fields[i].native_type and fields[i].size for
 * nested structs before calling. For non-STRUCT fields, size is
 * auto-computed from native_type. */
XR_FUNC void xr_aggregate_layout_compute(XrAggregateLayout *layout);

/*
 * Static layout query for C-porting surfaces.
 *
 * Returns true only when the type has an unambiguous low-level layout:
 * native scalars, raw pointers, fixed arrays whose element is statically
 * laid out, and fixed-layout value structs. Managed references are
 * intentionally rejected for C ABI contracts even when they have a known
 * storage lane inside Xray aggregates.
 */
XR_FUNC bool xr_type_has_static_layout(const XrType *type, uint32_t *out_size, uint32_t *out_align);
XR_FUNC bool xr_type_has_static_field_offset(const XrType *type, const char *field_name,
                                             uint32_t *out_offset);

/*
 * Convert XrTypeKind + native_width to XrNativeType for value struct fields.
 * native_width stores XrNativeType directly (0 = use default for kind).
 * Returns -1 if the type is not valid for struct fields.
 *
 * Implemented in xstruct_layout.c to avoid pulling xtype.h into this
 * header (xtype.h already includes xstruct_layout.h, so a back-reference
 * here would form an include cycle).
 */
XR_FUNC int xr_type_kind_to_native(int kind, uint8_t native_width);

#endif  // XSTRUCT_LAYOUT_H
