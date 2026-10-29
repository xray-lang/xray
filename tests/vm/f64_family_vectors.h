/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * f64_family_vectors.h - Complete arithmetic and independent ordering witnesses
 */
#ifndef F64_FAMILY_VECTORS_H
#define F64_FAMILY_VECTORS_H
#include "xir/xxir_float.h"
static const struct {
    uint32_t width;
    XrXirFloatOperation operation;
    uint64_t left,right,expected;
} float_arithmetic_witnesses[] = {
#define XIR_FLOAT_ARITHMETIC(width, operation, a, b, result) {width,operation,a,b,result},
#include "../unit/xir/xir_float_arithmetic_vectors.def"
#undef XIR_FLOAT_ARITHMETIC
};
static const struct {uint64_t bits;int rank;} float_order_witnesses[] = {
#define XR_F64_VALUE(bits, rank) {UINT64_C(0x##bits),rank},
#include "f64_compare_values.def"
#undef XR_F64_VALUE
};
static uint64_t float_relation_mask(unsigned left,unsigned right) {
    int a=float_order_witnesses[left].rank,b=float_order_witnesses[right].rank;
    return a<0 || b<0 ? 2u : a==b ? 41u : a<b ? 14u : 50u;
}
#endif // F64_FAMILY_VECTORS_H
