/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLIR_ARRAY_SCALAR_REGION_H
#define XLIR_ARRAY_SCALAR_REGION_H
#include "xlir_internal.h"

/* These instructions can run with a private pending Array length. */
static inline bool lir_array_region_scalar(XrLirType type) {
    return type == XR_LIR_I1 || type == XR_LIR_I64;
}

static inline bool lir_array_region_pure(const XrLirFunctionData *f, const XrLirInstructionView *op) {
    if (!lir_array_region_scalar(op->type)) return false;
    switch (op->op) {
    case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
    case XR_LIR_NEG: case XR_LIR_ICMP: case XR_LIR_INTCAST: break;
    default: return false;
    }
    for (uint32_t a = 0; a < op->operand_count; ++a)
        if (!lir_array_region_scalar(f->values[op->operands[a] - 1].type)) return false;
    return true;
}
#endif // XLIR_ARRAY_SCALAR_REGION_H
