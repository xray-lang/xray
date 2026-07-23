/*
 * xray - Public scalar source spelling and semantic representation helpers.
 */

#ifndef XR_SCALAR_TYPE_H
#define XR_SCALAR_TYPE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "xr_native_type_core.h"

#define XR_SCALAR_REP_NONE UINT8_MAX

typedef enum {
#define XR_SCALAR_TYPE(source_id, spelling, length, lexer_token, scalar_rep, type_family, role,    \
                       canonical_display, public_type_id, range_class)                             \
    XR_SOURCE_TYPE_##source_id,
#include "xr_scalar_type.def"
#undef XR_SCALAR_TYPE
    XR_SOURCE_TYPE_COUNT,
    XR_SOURCE_TYPE_NONE = 255
} XrSourceTypeSpelling;

static inline bool xr_scalar_rep_is_integer(uint8_t scalar_rep) {
    switch ((XrNativeType) scalar_rep) {
        case XR_NATIVE_I8:
        case XR_NATIVE_U8:
        case XR_NATIVE_I16:
        case XR_NATIVE_U16:
        case XR_NATIVE_I32:
        case XR_NATIVE_U32:
        case XR_NATIVE_I64:
        case XR_NATIVE_U64:
        case XR_NATIVE_ISIZE:
        case XR_NATIVE_USIZE:
            return true;
        default:
            return false;
    }
}

static inline bool xr_scalar_rep_is_float(uint8_t scalar_rep) {
    return scalar_rep == XR_NATIVE_F32 || scalar_rep == XR_NATIVE_F64;
}

static inline bool xr_scalar_rep_is_unsigned(uint8_t scalar_rep) {
    return scalar_rep == XR_NATIVE_U8 || scalar_rep == XR_NATIVE_U16 ||
           scalar_rep == XR_NATIVE_U32 || scalar_rep == XR_NATIVE_U64 ||
           scalar_rep == XR_NATIVE_USIZE;
}

static inline const char *xr_source_type_spelling_name(XrSourceTypeSpelling source) {
    switch (source) {
#define XR_SCALAR_TYPE(source_id, spelling, length, lexer_token, scalar_rep, type_family, role,    \
                       canonical_display, public_type_id, range_class)                             \
    case XR_SOURCE_TYPE_##source_id:                                                               \
        return spelling;
#include "xr_scalar_type.def"
#undef XR_SCALAR_TYPE
        default:
            return NULL;
    }
}

static inline uint8_t xr_source_type_scalar_rep(XrSourceTypeSpelling source) {
    switch (source) {
#define XR_SCALAR_TYPE(source_id, spelling, length, lexer_token, scalar_rep, type_family, role,    \
                       canonical_display, public_type_id, range_class)                             \
    case XR_SOURCE_TYPE_##source_id:                                                               \
        return scalar_rep;
#include "xr_scalar_type.def"
#undef XR_SCALAR_TYPE
        default:
            return UINT8_MAX;
    }
}

static inline const char *xr_scalar_rep_canonical_name(uint8_t scalar_rep) {
    switch ((XrNativeType) scalar_rep) {
        case XR_NATIVE_I8:
            return "i8";
        case XR_NATIVE_U8:
            return "byte";
        case XR_NATIVE_I16:
            return "i16";
        case XR_NATIVE_U16:
            return "u16";
        case XR_NATIVE_I32:
            return "i32";
        case XR_NATIVE_U32:
            return "u32";
        case XR_NATIVE_I64:
            return "int";
        case XR_NATIVE_U64:
            return "u64";
        case XR_NATIVE_ISIZE:
            return "isize";
        case XR_NATIVE_USIZE:
            return "usize";
        case XR_NATIVE_F32:
            return "f32";
        case XR_NATIVE_F64:
            return "float";
        default:
            return NULL;
    }
}

static inline XrSourceTypeSpelling xr_source_type_spelling_lookup(const char *name, size_t len) {
    if (!name)
        return XR_SOURCE_TYPE_NONE;
#define XR_SCALAR_TYPE(source_id, spelling, length, lexer_token, scalar_rep, type_family, role,    \
                       canonical_display, public_type_id, range_class)                             \
    if (len == (size_t) length && memcmp(name, spelling, (size_t) length) == 0)                    \
        return XR_SOURCE_TYPE_##source_id;
#include "xr_scalar_type.def"
#undef XR_SCALAR_TYPE
    return XR_SOURCE_TYPE_NONE;
}

#endif
