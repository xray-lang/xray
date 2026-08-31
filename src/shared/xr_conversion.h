/*
 * xray - Canonical numeric/dynamic conversion classification.
 *
 * This header deliberately contains no AST or analyzer pointers.  The compact
 * witness can cross parser/analyzer/TypedProgram/Xi boundaries without
 * retaining storage owned by an earlier stage.
 */

#ifndef XR_CONVERSION_H
#define XR_CONVERSION_H

#include <stdbool.h>
#include <stdint.h>

#include "../base/xdefs.h"

typedef enum XrConversionKind {
    XR_CONVERSION_NONE = 0,
    XR_CONVERSION_IDENTITY,
    XR_CONVERSION_CONTEXTUAL_LITERAL,
    XR_CONVERSION_LOSSLESS_WIDEN,
    XR_CONVERSION_EXPLICIT_TRUNCATE,
    XR_CONVERSION_EXPLICIT_SIGN_CHANGE,
    XR_CONVERSION_EXPLICIT_TARGET_WIDTH,
    XR_CONVERSION_EXPLICIT_INT_FLOAT,
    XR_CONVERSION_ENUM_ORDINAL,
    XR_CONVERSION_DYNAMIC_CHECKED,
    XR_CONVERSION_DYNAMIC_NULLABLE,
    XR_CONVERSION_DISALLOWED,
} XrConversionKind;

typedef struct XrConversionWitness {
    XrConversionKind kind;
    uint8_t source_scalar_rep;
    uint8_t target_scalar_rep;
    bool is_implicit;
    bool is_compile_time;
} XrConversionWitness;

static inline bool xr_conversion_kind_is_numeric(XrConversionKind kind);

/* Enum ordinal conversion accepts only native integer destinations.  VM,
 * verifier, and C emission share this classifier so malformed evidence cannot
 * select a different destination domain in a later stage. */
XR_FUNC bool xr_conversion_scalar_rep_is_integer(uint8_t scalar_rep);

/* Compact VM-bytecode evidence for statically classified conversion. Numeric
 * conversion and text parsing are disjoint modes; zero has no public source
 * conversion meaning. */
#define XR_CONVERSION_BC_PRESENT UINT16_C(0x8000)
#define XR_CONVERSION_BC_PARSE_REQUIRED UINT16_C(0x4000)
#define XR_CONVERSION_BC_PARSE_OPTIONAL UINT16_C(0x2000)
/* This multi-bit tag is intentionally outside every exact parse mode. It
 * decodes as nonnumeric kind 9, and injecting PRESENT remains nonnumeric. */
#define XR_CONVERSION_BC_ENUM_ORDINAL UINT16_C(0x4800)
#define XR_CONVERSION_BC_POINTER32 UINT16_C(0x0400)

static inline uint16_t xr_conversion_bytecode_pack(const XrConversionWitness *witness,
                                                   uint8_t pointer_bits) {
    if (!witness)
        return 0;
    if (witness->kind == XR_CONVERSION_ENUM_ORDINAL) {
        if (witness->source_scalar_rep != UINT8_MAX ||
            !xr_conversion_scalar_rep_is_integer(witness->target_scalar_rep) ||
            witness->is_implicit || witness->is_compile_time)
            return 0;
        return (uint16_t) (XR_CONVERSION_BC_ENUM_ORDINAL |
                           (pointer_bits == 32 ? XR_CONVERSION_BC_POINTER32 : 0) |
                           (((uint16_t) witness->target_scalar_rep & UINT16_C(0x1f)) << 5));
    }
    if (!xr_conversion_kind_is_numeric(witness->kind))
        return 0;
    return (uint16_t) (XR_CONVERSION_BC_PRESENT |
                       ((uint16_t) witness->source_scalar_rep & UINT16_C(0x1f)) |
                       (((uint16_t) witness->target_scalar_rep & UINT16_C(0x1f)) << 5) |
                       (pointer_bits == 32 ? XR_CONVERSION_BC_POINTER32 : 0) |
                       (((uint16_t) witness->kind & UINT16_C(0x0f)) << 11));
}

/* Numeric kind bits share the upper payload region with parse tags.  PRESENT
 * is therefore the mode discriminator: parse tags are valid only when the
 * numeric witness bit is clear. */
static inline bool xr_conversion_bytecode_is_parse_required(uint16_t packed) {
    return packed == XR_CONVERSION_BC_PARSE_REQUIRED;
}

static inline bool xr_conversion_bytecode_is_parse_optional(uint16_t packed) {
    return packed == XR_CONVERSION_BC_PARSE_OPTIONAL;
}

static inline uint8_t xr_conversion_bytecode_source_rep(uint16_t packed) {
    return (uint8_t) (packed & UINT16_C(0x1f));
}

static inline uint8_t xr_conversion_bytecode_target_rep(uint16_t packed) {
    return (uint8_t) ((packed >> 5) & UINT16_C(0x1f));
}

static inline uint8_t xr_conversion_bytecode_pointer_bits(uint16_t packed) {
    return (packed & XR_CONVERSION_BC_POINTER32) != 0 ? 32 : 64;
}

static inline bool xr_conversion_bytecode_is_enum_ordinal(uint16_t packed) {
    const uint16_t allowed =
        XR_CONVERSION_BC_ENUM_ORDINAL | XR_CONVERSION_BC_POINTER32 | UINT16_C(0x03e0);
    /* Enum ordinal mode has no source representation payload and accepts only
     * an exact native integer destination representation. */
    return (packed & XR_CONVERSION_BC_ENUM_ORDINAL) == XR_CONVERSION_BC_ENUM_ORDINAL &&
           (packed & ~allowed) == 0 &&
           xr_conversion_scalar_rep_is_integer(xr_conversion_bytecode_target_rep(packed));
}

static inline XrConversionKind xr_conversion_bytecode_kind(uint16_t packed) {
    if (xr_conversion_bytecode_is_enum_ordinal(packed))
        return XR_CONVERSION_ENUM_ORDINAL;
    return (XrConversionKind) ((packed >> 11) & UINT16_C(0x0f));
}

/* Numeric mode requires PRESENT and one of the exact numeric conversion kinds.
 * Parse and enum-ordinal encodings never set a valid numeric mode. */
static inline bool xr_conversion_bytecode_is_numeric(uint16_t packed) {
    return (packed & XR_CONVERSION_BC_PRESENT) != 0 &&
           xr_conversion_kind_is_numeric(xr_conversion_bytecode_kind(packed));
}

static inline bool xr_conversion_kind_is_implicit(XrConversionKind kind) {
    return kind == XR_CONVERSION_IDENTITY || kind == XR_CONVERSION_CONTEXTUAL_LITERAL ||
           kind == XR_CONVERSION_LOSSLESS_WIDEN;
}

static inline bool xr_conversion_kind_is_numeric(XrConversionKind kind) {
    return kind >= XR_CONVERSION_IDENTITY && kind <= XR_CONVERSION_EXPLICIT_INT_FLOAT;
}

XR_FUNC const char *xr_conversion_kind_name(XrConversionKind kind);

#endif /* XR_CONVERSION_H */
