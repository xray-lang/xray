/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_math_lower.inc.c - Checked standard math scalar leaves
 *
 * KEY CONCEPT:
 *   Both execution targets use the verified scalar operations and range panic.
 */
#include "../shared/xr_numeric_conversion_core.h"

static uint32_t lower_math_native_id(const Lower *c, uint32_t index) {
    const XrXirFunctionIdentity *identity = &c->source->declarations->functions[index];
    switch (identity->native_callable) {
    case XR_NATIVE_CALLABLE_MATH_ABS:
    case XR_NATIVE_CALLABLE_MATH_FLOOR:
    case XR_NATIVE_CALLABLE_MATH_CEIL:
    case XR_NATIVE_CALLABLE_MATH_ROUND:
    case XR_NATIVE_CALLABLE_MATH_TRUNC:
    case XR_NATIVE_CALLABLE_MATH_SIN:
    case XR_NATIVE_CALLABLE_MATH_COS:
    case XR_NATIVE_CALLABLE_MATH_SQRT:
    case XR_NATIVE_CALLABLE_MATH_HYPOT:
        return !identity->capture_owner && !identity->capture_count &&
            xir_native_callable_verify(c->source, index) == XR_XIR_OK ? identity->native_callable : 0;
    default:
        return 0;
    }
}

static bool lower_math_unary_entry(Lower *c, XrLirFunction function, XrLirBlock *entry, XrLirValue *input) {
    return lower_build(c, xr_lir_define_function(c->module, function)) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, entry)) &&
        lower_build(c, xr_lir_set_entry(c->module, function, *entry)) &&
        lower_build(c, xr_lir_function_parameter(c->module, function, 0, input));
}

static bool lower_math_return(Lower *c, XrLirBlock block, XrLirValue result) {
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&result, .operand_count=1};
    return lower_build(c, xr_lir_set_terminator(c->module, block, &ret));
}

static XrLirExternKind lower_math_external_kind(uint32_t id) {
    switch (id) {
    case XR_NATIVE_CALLABLE_MATH_SIN: return XR_LIR_EXTERN_MATH_SIN;
    case XR_NATIVE_CALLABLE_MATH_COS: return XR_LIR_EXTERN_MATH_COS;
    case XR_NATIVE_CALLABLE_MATH_SQRT: return XR_LIR_EXTERN_MATH_SQRT;
    case XR_NATIVE_CALLABLE_MATH_HYPOT: return XR_LIR_EXTERN_MATH_HYPOT;
    default: return XR_LIR_EXTERN_NONE;
    }
}

static bool lower_math_external_body(Lower *c, XrLirFunction function, XrLirExternKind kind) {
    XrLirMathExternal shape = xr_lir_math_external(kind);
    const XrLirType types[] = {XR_LIR_F64, XR_LIR_F64};
    XrLirFunction target = {0};
    XrLirBlock entry = {0};
    XrLirValue arguments[2] = {{0}}, result = {0};
    XrLirFunctionSpec spec = {.name=shape.name, .name_length=strlen(shape.name), .parameters=types,
        .parameter_count=shape.parameter_count, .result=XR_LIR_F64,
        .kind=XR_LIR_FUNCTION_EXTERNAL, .extern_kind=kind};
    if (!lower_build(c, xr_lir_declare_function(c->module, &spec, &target)) ||
        !lower_math_unary_entry(c, function, &entry, &arguments[0]) ||
        (shape.parameter_count == 2 && !lower_build(c,
            xr_lir_function_parameter(c->module, function, 1, &arguments[1])))) return false;
    XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_F64, .callee=target,
        .operand_count=shape.parameter_count, .operands=arguments, .source=lower_source(c)};
    return lower_build(c, xr_lir_emit_value(c->module, entry, &call, &result)) &&
        lower_math_return(c, entry, result);
}

static bool lower_math_round_adjust(Lower *c, XrLirBlock block, XrLirValue integral, XrLirOp op) {
    XrLirValue operands[2] = {integral, {0}}, result = {0};
    XrLirInstruction one = {.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=1};
    XrLirInstruction adjust = {.op=op, .type=XR_LIR_I64, .operands=operands, .operand_count=2};
    return lower_build(c, xr_lir_emit_value(c->module, block, &one, &operands[1])) &&
        lower_build(c, xr_lir_emit_value(c->module, block, &adjust, &result)) &&
        lower_math_return(c, block, result);
}

static bool lower_math_branch(Lower *c, XrLirBlock block, XrLirValue left, XrLirValue right,
    XrLirPredicate predicate, XrLirBlock when_true, XrLirBlock otherwise) {
    XrLirValue operands[2] = {left, right}, selected = {0};
    XrLirInstruction compare = {.op=XR_LIR_FCMP, .type=XR_LIR_I1, .predicate=predicate,
        .operands=operands, .operand_count=2};
    XrLirInstruction branch = {.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&selected,
        .operand_count=1, .edge_count=2, .edges={{.target=when_true}, {.target=otherwise}}};
    return lower_build(c, xr_lir_emit_value(c->module, block, &compare, &selected)) &&
        lower_build(c, xr_lir_set_terminator(c->module, block, &branch));
}

static bool lower_math_round_branch(Lower *c, XrLirBlock block, XrLirValue fraction,
    uint64_t threshold, XrLirPredicate predicate, XrLirBlock adjust, XrLirBlock otherwise) {
    XrLirValue limit = {0};
    XrLirInstruction constant = {.op=XR_LIR_CONST, .type=XR_LIR_F64, .bits=threshold};
    return lower_build(c, xr_lir_emit_value(c->module, block, &constant, &limit)) &&
        lower_math_branch(c, block, fraction, limit, predicate, adjust, otherwise);
}

static bool lower_math_abs_body(Lower *c, XrLirFunction function) {
    XrLirBlock entry = {0}, positive = {0}, test_negative = {0}, negative = {0};
    XrLirBlock test_zero = {0}, zero = {0}, unordered = {0};
    XrLirValue input = {0}, positive_zero = {0}, magnitude = {0}, canonical_nan = {0};
    XrLirInstruction zero_constant = {.op=XR_LIR_CONST, .type=XR_LIR_F64};
    XrLirInstruction negate = {.op=XR_LIR_FNEG, .type=XR_LIR_F64, .operands=&input, .operand_count=1};
    XrLirInstruction nan_constant = {.op=XR_LIR_CONST, .type=XR_LIR_F64, .bits=XR_NUMERIC_CANONICAL_F64_NAN};

    /* Ordered comparisons separate both zero signs and NaNs from the two
     * nonzero signs. The NaN result matches the checked math evaluator. */
    return lower_math_unary_entry(c, function, &entry, &input) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &positive)) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &test_negative)) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &negative)) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &test_zero)) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &zero)) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &unordered)) &&
        lower_build(c, xr_lir_emit_value(c->module, entry, &zero_constant, &positive_zero)) &&
        lower_math_branch(c, entry, input, positive_zero, XR_LIR_GT, positive, test_negative) &&
        lower_math_branch(c, test_negative, input, positive_zero, XR_LIR_LT, negative, test_zero) &&
        lower_math_branch(c, test_zero, input, positive_zero, XR_LIR_EQ, zero, unordered) &&
        lower_math_return(c, positive, input) && lower_math_return(c, zero, positive_zero) &&
        lower_build(c, xr_lir_emit_value(c->module, negative, &negate, &magnitude)) &&
        lower_math_return(c, negative, magnitude) &&
        lower_build(c, xr_lir_emit_value(c->module, unordered, &nan_constant, &canonical_nan)) &&
        lower_math_return(c, unordered, canonical_nan);
}

static bool lower_math_rounding_body(Lower *c, XrLirFunction function, uint32_t id) {
    XrLirBlock entry = {0}, unchanged = {0}, adjust = {0};
    XrLirValue input = {0}, integral = {0}, back = {0}, fraction = {0};
    XrLirInstruction to_int = {.op=XR_LIR_FTOI, .type=XR_LIR_I64, .source=lower_source(c),
        .operands=&input, .operand_count=1};
    if (!lower_math_unary_entry(c, function, &entry, &input) ||
        !lower_build(c, xr_lir_emit_value(c->module, entry, &to_int, &integral))) return false;
    if (id == XR_NATIVE_CALLABLE_MATH_TRUNC)
        return lower_math_return(c, entry, integral);

    /* Subtract the exact integral part before testing for a half. With a
     * nonzero fraction the magnitude is below 2^52, so the adjustment fits. */
    XrLirValue difference[] = {input, {0}};
    XrLirInstruction to_float = {.op=XR_LIR_ITOF, .type=XR_LIR_F64, .operands=&integral, .operand_count=1};
    XrLirInstruction subtract = {.op=XR_LIR_FSUB, .type=XR_LIR_F64, .operands=difference, .operand_count=2};
    if (!lower_build(c, xr_lir_emit_value(c->module, entry, &to_float, &back))) return false;
    difference[1] = back;
    if (!lower_build(c, xr_lir_emit_value(c->module, entry, &subtract, &fraction)) ||
        !lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &unchanged)) ||
        !lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &adjust)) ||
        !lower_math_return(c, unchanged, integral)) return false;

    if (id != XR_NATIVE_CALLABLE_MATH_ROUND) {
        bool floor = id == XR_NATIVE_CALLABLE_MATH_FLOOR;
        return lower_math_round_branch(c, entry, fraction, 0, floor ? XR_LIR_LT : XR_LIR_GT, adjust, unchanged) &&
            lower_math_round_adjust(c, adjust, integral, floor ? XR_LIR_SUB : XR_LIR_ADD);
    }

    XrLirBlock test_negative = {0}, decrease = {0};
    return lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &test_negative)) &&
        lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &decrease)) &&
        lower_math_round_branch(c, entry, fraction, UINT64_C(0x3fe0000000000000), XR_LIR_GE, adjust, test_negative) &&
        lower_math_round_branch(c, test_negative, fraction, UINT64_C(0xbfe0000000000000), XR_LIR_LE, decrease, unchanged) &&
        lower_math_round_adjust(c, adjust, integral, XR_LIR_ADD) &&
        lower_math_round_adjust(c, decrease, integral, XR_LIR_SUB);
}
