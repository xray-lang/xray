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

typedef enum XrtValueKind {
    XRT_VALUE_UNIT, XRT_VALUE_BOOL, XRT_VALUE_RUNE,
    XRT_VALUE_I8, XRT_VALUE_I16, XRT_VALUE_I32, XRT_VALUE_I64,
    XRT_VALUE_U8, XRT_VALUE_U16, XRT_VALUE_U32, XRT_VALUE_U64,
    XRT_VALUE_F32, XRT_VALUE_F64, XRT_VALUE_STRING,
    XRT_VALUE_ARRAY_I64, XRT_VALUE_ARRAY_STRING, XRT_VALUE_ARRAY_U8, XRT_VALUE_ARRAY_U64,
    XRT_VALUE_MAP_STRING_I64, XRT_VALUE_FIXED_U8, XRT_VALUE_TUPLE_I64,
    XRT_VALUE_RECORD, XRT_VALUE_NULLABLE, XRT_VALUE_FUNCTION, XRT_VALUE_CELL,
    XRT_VALUE_ENVIRONMENT
} XrtValueKind;
enum { XRT_VALUE_NO_SUSPEND = 1u, XRT_VALUE_C_ABI = 2u };

/* Type references are one-based; zero means absent. A type depends only on
 * earlier rows. Vector spans are zero-based. Empty spans start at zero. Names
 * are byte spans, without an implied trailing NUL. These native C descriptors
 * are not a wire format or permission to invoke an entry ordinal. */
typedef struct XrtValueImageSlot {
    uint32_t type;
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
} XrtValueImageType;
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
} XrtValueImage;
#endif // XRAY_NATIVE_VALUE_IMAGE_H
