/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_exact_scalar_registry.h - Exact public scalar semantic identities.
 */

#ifndef XR_EXACT_SCALAR_REGISTRY_H
#define XR_EXACT_SCALAR_REGISTRY_H

#include "../base/xdefs.h"
#include "xr_native_type_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum XrExactScalarId {
    XR_EXACT_SCALAR_NONE = 0,
#define XR_EXACT_SCALAR(id, stable_id, source_name, native_type, family, range_class, flags)      \
    XR_EXACT_SCALAR_##id = stable_id,
#include "xr_exact_scalar_registry.def"
#undef XR_EXACT_SCALAR
} XrExactScalarId;

typedef enum XrExactScalarFamily {
    XR_EXACT_SCALAR_FAMILY_INTEGER = 1,
    XR_EXACT_SCALAR_FAMILY_FLOAT = 2
} XrExactScalarFamily;

typedef enum XrExactScalarRangeClass {
    XR_EXACT_SCALAR_RANGE_SIGNED = 1,
    XR_EXACT_SCALAR_RANGE_UNSIGNED = 2,
    XR_EXACT_SCALAR_RANGE_FLOATING = 3
} XrExactScalarRangeClass;

enum {
    XR_EXACT_SCALAR_FLAG_NONE = 0,
    XR_EXACT_SCALAR_FLAG_DEFAULT_INTEGER = 1u << 0,
    XR_EXACT_SCALAR_FLAG_DEFAULT_DECIMAL = 1u << 1,
    XR_EXACT_SCALAR_FLAG_BYTE_ELEMENT = 1u << 2,
    XR_EXACT_SCALAR_FLAG_TARGET_WIDTH = 1u << 3
};

typedef struct XrExactScalarDesc {
    XrExactScalarId id;
    const char *source_name;
    uint8_t source_length;
    uint8_t native_type;
    XrExactScalarFamily family;
    XrExactScalarRangeClass range_class;
    uint8_t flags;
} XrExactScalarDesc;

static inline const XrExactScalarDesc *xr_exact_scalar_rows(size_t *count) {
    static const XrExactScalarDesc rows[] = {
#define XR_EXACT_SCALAR(id, stable_id, source_name, native_type, family, range_class, flags)      \
    {XR_EXACT_SCALAR_##id, source_name, (uint8_t) (sizeof(source_name) - 1), native_type,          \
     XR_EXACT_SCALAR_FAMILY_##family, XR_EXACT_SCALAR_RANGE_##range_class,                        \
     XR_EXACT_SCALAR_FLAG_##flags},
#include "xr_exact_scalar_registry.def"
#undef XR_EXACT_SCALAR
    };
    if (count)
        *count = sizeof(rows) / sizeof(rows[0]);
    return rows;
}

static inline size_t xr_exact_scalar_count(void) {
    size_t count = 0;
    (void) xr_exact_scalar_rows(&count);
    return count;
}

static inline const XrExactScalarDesc *xr_exact_scalar_at(size_t index) {
    size_t count = 0;
    const XrExactScalarDesc *rows = xr_exact_scalar_rows(&count);
    return index < count ? &rows[index] : NULL;
}

static inline const XrExactScalarDesc *xr_exact_scalar_by_id(XrExactScalarId id) {
    size_t count = 0;
    const XrExactScalarDesc *rows = xr_exact_scalar_rows(&count);
    for (size_t i = 0; i < count; i++) {
        if (rows[i].id == id)
            return &rows[i];
    }
    return NULL;
}

static inline const XrExactScalarDesc *xr_exact_scalar_by_native_type(uint8_t native_type) {
    size_t count = 0;
    const XrExactScalarDesc *rows = xr_exact_scalar_rows(&count);
    for (size_t i = 0; i < count; i++) {
        if (rows[i].native_type == native_type)
            return &rows[i];
    }
    return NULL;
}

static inline const XrExactScalarDesc *xr_exact_scalar_by_source_name(const char *name,
                                                                      size_t length) {
    size_t count = 0;
    const XrExactScalarDesc *rows = xr_exact_scalar_rows(&count);
    if (!name)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        if (rows[i].source_length == length && memcmp(rows[i].source_name, name, length) == 0)
            return &rows[i];
    }
    return NULL;
}

static inline void xr_exact_scalar_set_error(char *error, size_t error_size,
                                             const char *message) {
    if (!error || error_size == 0)
        return;
    size_t length = strlen(message);
    if (length >= error_size)
        length = error_size - 1;
    memcpy(error, message, length);
    error[length] = '\0';
}

static inline bool xr_exact_scalar_row_shape_is_valid(const XrExactScalarDesc *row) {
    if (!row->source_name || row->source_name[0] == '\0' ||
        strlen(row->source_name) != row->source_length || row->id == XR_EXACT_SCALAR_NONE)
        return false;
    if (row->family != XR_EXACT_SCALAR_FAMILY_INTEGER &&
        row->family != XR_EXACT_SCALAR_FAMILY_FLOAT)
        return false;
    if (row->range_class < XR_EXACT_SCALAR_RANGE_SIGNED ||
        row->range_class > XR_EXACT_SCALAR_RANGE_FLOATING)
        return false;
    if (row->family == XR_EXACT_SCALAR_FAMILY_FLOAT)
        return row->range_class == XR_EXACT_SCALAR_RANGE_FLOATING;
    return row->range_class != XR_EXACT_SCALAR_RANGE_FLOATING;
}

static inline bool xr_exact_scalar_row_flags_are_valid(const XrExactScalarDesc *row) {
    const uint8_t all_flags = XR_EXACT_SCALAR_FLAG_DEFAULT_INTEGER |
                              XR_EXACT_SCALAR_FLAG_DEFAULT_DECIMAL |
                              XR_EXACT_SCALAR_FLAG_BYTE_ELEMENT |
                              XR_EXACT_SCALAR_FLAG_TARGET_WIDTH;
    if ((row->flags & ~all_flags) != 0)
        return false;
    if ((row->flags & XR_EXACT_SCALAR_FLAG_DEFAULT_INTEGER) != 0 &&
        row->native_type != XR_NATIVE_I64)
        return false;
    if ((row->flags & XR_EXACT_SCALAR_FLAG_DEFAULT_DECIMAL) != 0 &&
        row->native_type != XR_NATIVE_F64)
        return false;
    if ((row->flags & XR_EXACT_SCALAR_FLAG_BYTE_ELEMENT) != 0 &&
        row->native_type != XR_NATIVE_U8)
        return false;
    if ((row->flags & XR_EXACT_SCALAR_FLAG_TARGET_WIDTH) != 0 &&
        row->native_type != XR_NATIVE_ISIZE && row->native_type != XR_NATIVE_USIZE)
        return false;
    return true;
}

static inline bool xr_exact_scalar_registry_validate(char *error, size_t error_size) {
    static const uint8_t required_native_types[] = {
        XR_NATIVE_I8,  XR_NATIVE_I16, XR_NATIVE_I32, XR_NATIVE_I64,
        XR_NATIVE_U8,  XR_NATIVE_U16, XR_NATIVE_U32, XR_NATIVE_U64,
        XR_NATIVE_F32, XR_NATIVE_F64, XR_NATIVE_ISIZE, XR_NATIVE_USIZE,
    };
    bool seen_ids[13] = {false};
    size_t count = 0;
    size_t default_integer_count = 0;
    size_t default_decimal_count = 0;
    size_t byte_element_count = 0;
    const XrExactScalarDesc *rows = xr_exact_scalar_rows(&count);
    const size_t required_count = sizeof(required_native_types) / sizeof(required_native_types[0]);

    if (count != required_count) {
        xr_exact_scalar_set_error(error, error_size, "exact scalar registry is not exhaustive");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        const XrExactScalarDesc *row = &rows[i];
        if (!xr_exact_scalar_row_shape_is_valid(row) ||
            !xr_exact_scalar_row_flags_are_valid(row)) {
            xr_exact_scalar_set_error(error, error_size,
                                      "exact scalar registry contains a dead row");
            return false;
        }
        if ((unsigned) row->id >= sizeof(seen_ids) || seen_ids[row->id]) {
            xr_exact_scalar_set_error(error, error_size,
                                      "exact scalar registry contains an invalid stable ID");
            return false;
        }
        seen_ids[row->id] = true;
        if (strcmp(row->source_name, "int") == 0 || strcmp(row->source_name, "byte") == 0 ||
            strcmp(row->source_name, "float") == 0) {
            xr_exact_scalar_set_error(error, error_size,
                                      "exact scalar registry contains a removed alias");
            return false;
        }
        for (size_t prior = 0; prior < i; prior++) {
            const XrExactScalarDesc *other = &rows[prior];
            if (row->native_type == other->native_type ||
                strcmp(row->source_name, other->source_name) == 0) {
                xr_exact_scalar_set_error(error, error_size,
                                          "exact scalar registry contains a duplicate");
                return false;
            }
        }
        default_integer_count +=
            (row->flags & XR_EXACT_SCALAR_FLAG_DEFAULT_INTEGER) != 0 ? 1u : 0u;
        default_decimal_count +=
            (row->flags & XR_EXACT_SCALAR_FLAG_DEFAULT_DECIMAL) != 0 ? 1u : 0u;
        byte_element_count +=
            (row->flags & XR_EXACT_SCALAR_FLAG_BYTE_ELEMENT) != 0 ? 1u : 0u;
    }
    for (size_t id = 1; id < sizeof(seen_ids); id++) {
        if (!seen_ids[id]) {
            xr_exact_scalar_set_error(error, error_size,
                                      "exact scalar registry omits a stable ID");
            return false;
        }
    }
    for (size_t i = 0; i < required_count; i++) {
        if (!xr_exact_scalar_by_native_type(required_native_types[i])) {
            xr_exact_scalar_set_error(error, error_size,
                                      "exact scalar registry omits a representation");
            return false;
        }
    }
    if (default_integer_count != 1 || default_decimal_count != 1 || byte_element_count != 1) {
        xr_exact_scalar_set_error(error, error_size,
                                  "exact scalar registry has ambiguous default roles");
        return false;
    }
    if (error && error_size > 0)
        error[0] = '\0';
    return true;
}

#endif /* XR_EXACT_SCALAR_REGISTRY_H */
