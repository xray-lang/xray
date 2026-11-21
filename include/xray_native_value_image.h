/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_native_value_image.h - Read-only physical descriptions for generated C
 *
 * KEY CONCEPT:
 *   Rows use indices and spans, never compiler or runtime-plan pointers.
 *   C images and bytecode images describe the same fixed value representation.
 */
#ifndef XRAY_NATIVE_VALUE_IMAGE_H
#define XRAY_NATIVE_VALUE_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#include "xray_native_managed_root.h"

typedef enum XrtValueKind {
    XRT_VALUE_UNIT, XRT_VALUE_BOOL, XRT_VALUE_RUNE,
    XRT_VALUE_I8, XRT_VALUE_I16, XRT_VALUE_I32, XRT_VALUE_I64,
    XRT_VALUE_U8, XRT_VALUE_U16, XRT_VALUE_U32, XRT_VALUE_U64,
    XRT_VALUE_F32, XRT_VALUE_F64, XRT_VALUE_STRING,
    XRT_VALUE_ARRAY_I64, XRT_VALUE_ARRAY_STRING, XRT_VALUE_ARRAY_U8, XRT_VALUE_ARRAY_U64,
    XRT_VALUE_MAP_STRING_I64, XRT_VALUE_FIXED_U8, XRT_VALUE_TUPLE,
    XRT_VALUE_RECORD, XRT_VALUE_NULLABLE, XRT_VALUE_FUNCTION, XRT_VALUE_CELL,
    XRT_VALUE_ENVIRONMENT, XRT_VALUE_SLICE_I64, XRT_VALUE_SLICE_U8, XRT_VALUE_SET_I64,
    XRT_VALUE_ARRAY_INLINE,
    XRT_VALUE_CLASS,
    /* A tagged sum keeps a compiler-assigned tag and one active variant
     * payload. P1 admits owner-free payloads only, so the plan's owned leaf
     * list stays empty and the tag selects a field range, never an owner. */
    XRT_VALUE_SUM
} XrtValueKind;
/* Mutability received from the Checked field for the admitted Class Array
 * path. A flag describes the field and leaves access to the checked operation. */
enum { XRT_VALUE_FIELD_MUTABLE = 1u };
static inline int xrt_value_kind_has_fields(XrtValueKind kind) {
    return kind == XRT_VALUE_RECORD || kind == XRT_VALUE_TUPLE;
}
static inline int xrt_value_kind_is_array(XrtValueKind kind) {
    return (kind >= XRT_VALUE_ARRAY_I64 && kind <= XRT_VALUE_ARRAY_U64) || kind == XRT_VALUE_ARRAY_INLINE;
}
enum { XRT_VALUE_NO_SUSPEND = 1u, XRT_VALUE_C_ABI = 2u };
/* Tuple and Nullable rows may carry a verified display closure. */
enum { XRT_VALUE_INLINE_DISPLAY = 4u };

static inline int xrt_value_kind_is_display_leaf(XrtValueKind kind) {
    return kind == XRT_VALUE_BOOL || kind == XRT_VALUE_I64 ||
        kind == XRT_VALUE_F64 || kind == XRT_VALUE_STRING;
}

/* Type references are one-based; zero means absent. A type depends only on
 * earlier rows. Vector spans are zero-based. Empty spans start at zero. Names
 * are byte spans, without an implied trailing NUL. These native C descriptors
 * are not a wire format or permission to invoke an entry ordinal. */
typedef struct XrtValueImageSlot {
    uint32_t type, flags;
    size_t offset, name_offset, name_length;
} XrtValueImageSlot;
typedef struct XrtValueImageParameter {
    /* Modes are the admitted parameter contract: read=0, ref=1, move=2. */
    uint32_t type, mode, escaping;
    size_t name_offset, name_length;
} XrtValueImageParameter;
typedef struct XrtValueImageType {
    XrtValueKind kind;
    size_t size, alignment, count;
    uint32_t first_owned, owned_count;
    uint32_t first_field, field_count, first_capture, capture_count;
    uint32_t first_parameter, parameter_count;
    uint32_t nullable_element, result, flags, throws;
    size_t nullable_tag, nullable_payload;
    uint32_t entry_ordinal, entry_signature, element;
    uint32_t class_parent; /* Earlier concrete class row, zero for a root. */
    /* Tagged sum: tag bytes precede one overlapping variant payload. Variant
     * rows index the same flattened field span as records and tuples. */
    size_t sum_tag, sum_tag_size, sum_payload;
    uint32_t first_variant, variant_count;
} XrtValueImageType;
typedef struct XrtValueImageVariant {
    uint32_t first_field, field_count;
} XrtValueImageVariant;
typedef struct XrtValueImage {
    const XrtValueImageType *types;
    uint32_t type_count;
    const XrtValueImageSlot *slots;
    uint32_t slot_count;
    const XrtValueImageParameter *parameters;
    uint32_t parameter_count;
    const size_t *owned_offsets;
    uint32_t owned_count;
    const char *names;
    size_t name_bytes;
    const XrtValueImageVariant *variants;
    uint32_t variant_count;
} XrtValueImage;

/* A direct borrow of one sealed physical description, without an image lookup
 * or another set of payload owners. Offsets are relative to the value bytes
 * and describe every Header leaf, including inactive Nullable payloads. */
typedef struct XrtInlineValueSpec {
    size_t size, alignment;
    const size_t *owned_offsets;
    size_t owned_count;
} XrtInlineValueSpec;

/* Both byte spans and their owner slots are real homes in the same stable
 * Frame. The distinct temporary has true NULL leaves on entry and return.
 * Metadata and source bytes stay borrowed and immutable throughout the call.
 * These addresses supply storage, never mutation authority. */
typedef struct XrtInlineValueHome {
    XrtManagedFrame *frame;
    void *value, *temporary;
    size_t first_owner, temporary_first_owner;
} XrtInlineValueHome;

#ifdef __cplusplus
extern "C" {
#endif
/* Copy a caller-admitted complete natural value, including self-assignment.
 * NULL selects only a producer-confirmed cleanup or Nullable NONE, not a new
 * readable empty value of an otherwise nonnullable type. Every incoming leaf
 * is retained into real staging storage
 * before any destination changes. All leaves and scalar/tag bytes publish
 * before old owners retire, while the actual Root rejects reentry. Failure
 * preserves the destination and empties staging before returning normally.
 * No allocation occurs. Owner-free POD spans need no Frame or temporary. */
XRAY_API XrtManagedStatus xrt_inline_value_replace(const XrtInlineValueHome *,
    const XrtInlineValueSpec *, const void *source);
#ifdef __cplusplus
}
#endif
#endif // XRAY_NATIVE_VALUE_IMAGE_H
