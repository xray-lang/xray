/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_c_emission_schema.h - Self-contained dumb C emitter scalar schema
 *
 * KEY CONCEPT:
 *   This header contains only immutable C emission records. A text emitter can
 *   consume it without access to semantic plans, compiler IR, analyzer types,
 *   or the legacy AOT representation model.
 */

#ifndef XR_C_EMISSION_SCHEMA_H
#define XR_C_EMISSION_SCHEMA_H

#include <stdint.h>

typedef enum XrCScalarRep {
    XR_C_SCALAR_REP_VOID = 0,
    XR_C_SCALAR_REP_I8,
    XR_C_SCALAR_REP_U8,
    XR_C_SCALAR_REP_I16,
    XR_C_SCALAR_REP_U16,
    XR_C_SCALAR_REP_I32,
    XR_C_SCALAR_REP_U32,
    XR_C_SCALAR_REP_I64,
    XR_C_SCALAR_REP_U64,
    XR_C_SCALAR_REP_ISIZE,
    XR_C_SCALAR_REP_USIZE,
    XR_C_SCALAR_REP_F32,
    XR_C_SCALAR_REP_F64,
    XR_C_SCALAR_REP_BOOL,
    XR_C_SCALAR_REP_RUNE,
    XR_C_SCALAR_REP_COUNT,
} XrCScalarRep;

typedef struct XrCScalarEmissionView {
    uint32_t semantic_value;
    uint16_t target_register_rep;
    uint16_t target_memory_rep;
    uint16_t target_register_kind;
    uint16_t target_memory_kind;
    uint16_t register_bits;
    uint16_t memory_align;
    uint32_t memory_size;
    uint8_t rep;
    const char *c_type;
} XrCScalarEmissionView;

#endif  // XR_C_EMISSION_SCHEMA_H
