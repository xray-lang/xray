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

typedef enum XrConversionKind {
    XR_CONVERSION_NONE = 0,
    XR_CONVERSION_IDENTITY,
    XR_CONVERSION_CONTEXTUAL_LITERAL,
    XR_CONVERSION_LOSSLESS_WIDEN,
    XR_CONVERSION_EXPLICIT_TRUNCATE,
    XR_CONVERSION_EXPLICIT_SIGN_CHANGE,
    XR_CONVERSION_EXPLICIT_TARGET_WIDTH,
    XR_CONVERSION_EXPLICIT_INT_FLOAT,
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

/* Compact VM-bytecode evidence for statically classified numeric XI_CONVERT.
 * Zero remains reserved for the dynamic built-in int()/float() conversions. */
#define XR_CONVERSION_BC_PRESENT UINT16_C(0x8000)
#define XR_CONVERSION_BC_POINTER32 UINT16_C(0x0400)

static inline uint16_t xr_conversion_bytecode_pack(const XrConversionWitness *witness,
                                                   uint8_t pointer_bits) {
    if (!witness || !xr_conversion_kind_is_numeric(witness->kind))
        return 0;
    return (uint16_t) (XR_CONVERSION_BC_PRESENT |
                       ((uint16_t) witness->source_scalar_rep & UINT16_C(0x1f)) |
                       (((uint16_t) witness->target_scalar_rep & UINT16_C(0x1f)) << 5) |
                       (pointer_bits == 32 ? XR_CONVERSION_BC_POINTER32 : 0) |
                       (((uint16_t) witness->kind & UINT16_C(0x0f)) << 11));
}

static inline bool xr_conversion_bytecode_is_numeric(uint16_t packed) {
    return (packed & XR_CONVERSION_BC_PRESENT) != 0;
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

static inline XrConversionKind xr_conversion_bytecode_kind(uint16_t packed) {
    return (XrConversionKind) ((packed >> 11) & UINT16_C(0x0f));
}

static inline bool xr_conversion_kind_is_implicit(XrConversionKind kind) {
    return kind == XR_CONVERSION_IDENTITY || kind == XR_CONVERSION_CONTEXTUAL_LITERAL ||
           kind == XR_CONVERSION_LOSSLESS_WIDEN;
}

static inline bool xr_conversion_kind_is_numeric(XrConversionKind kind) {
    return kind >= XR_CONVERSION_IDENTITY && kind <= XR_CONVERSION_EXPLICIT_INT_FLOAT;
}

const char *xr_conversion_kind_name(XrConversionKind kind);

#endif /* XR_CONVERSION_H */
