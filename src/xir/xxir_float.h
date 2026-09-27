/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_float.h - Explicit binary floating conversion and ordering
 *
 * KEY CONCEPT:
 *   Integer bit operations preserve semantics independently of the host FPU.
 */
#ifndef XXIR_FLOAT_H
#define XXIR_FLOAT_H
#include "xxir_scalar.h"

typedef enum XrXirNumericStatus {
    XR_XIR_NUMERIC_OK, XR_XIR_NUMERIC_BAD_ARGUMENT, XR_XIR_NUMERIC_RANGE
} XrXirNumericStatus;
typedef enum XrXirFloatOrder {
    XR_XIR_FLOAT_LESS = -1, XR_XIR_FLOAT_EQUAL, XR_XIR_FLOAT_GREATER, XR_XIR_FLOAT_UNORDERED
} XrXirFloatOrder;

/* Widths are 32/64; binary32 uses the low bits and requires zero high bits. */
XR_FUNC XrXirNumericStatus xr_xir_float_convert(uint32_t source_bits, uint32_t target_bits,
    uint64_t input, uint64_t *output);
XR_FUNC XrXirNumericStatus xr_xir_integer_to_float(XrXirIntegerFormat source,
    uint32_t target_bits, int64_t input, uint64_t *output);
XR_FUNC XrXirNumericStatus xr_xir_float_to_integer(uint32_t source_bits,
    XrXirIntegerFormat target, uint64_t input, int64_t *output);
XR_FUNC XrXirNumericStatus xr_xir_float_compare(uint32_t bits,
    uint64_t left, uint64_t right, XrXirFloatOrder *output);
XR_FUNC XrXirNumericStatus xr_xir_float_negate(uint32_t bits, uint64_t input, uint64_t *output);
typedef enum XrXirFloatOperation {
    XR_XIR_FLOAT_ADD, XR_XIR_FLOAT_SUBTRACT, XR_XIR_FLOAT_MULTIPLY, XR_XIR_FLOAT_DIVIDE
} XrXirFloatOperation;
XR_FUNC XrXirNumericStatus xr_xir_float_arithmetic(uint32_t bits, XrXirFloatOperation operation,
    uint64_t left, uint64_t right, uint64_t *output);
XR_FUNC bool xr_xir_float_format(uint32_t bits, uint64_t input,
    char *bytes, size_t capacity, size_t *length);
typedef enum XrXirFloatRelation {
    XR_XIR_FLOAT_EQ, XR_XIR_FLOAT_NE, XR_XIR_FLOAT_LT, XR_XIR_FLOAT_LE, XR_XIR_FLOAT_GT, XR_XIR_FLOAT_GE
} XrXirFloatRelation;
XR_FUNC XrXirRunStatus xr_xir_number_convert(XrXirType source, XrXirType target, int64_t input, int64_t *output);
XR_FUNC XrXirRunStatus xr_xir_float_relation(XrXirType type, XrXirFloatRelation relation,
    int64_t left, int64_t right, int64_t *output);
XR_FUNC XrXirRunStatus xr_xir_float_negative(XrXirType type, int64_t input, int64_t *output);
XR_FUNC XrXirRunStatus xr_xir_float_binary(XrXirType type, XrXirFloatOperation operation,
    int64_t left, int64_t right, int64_t *output);
#endif
