/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_math_lower.inc.c - Checked standard math conversion leaves
 *
 * KEY CONCEPT:
 *   Both execution targets use the verified scalar operations and range panic.
 */
static uint32_t lower_math_rounding_id(const Lower *c, uint32_t index) {
    const XrXirFunctionIdentity *identity = &c->source->declarations->functions[index];
    switch (identity->native_callable) {
    case XR_NATIVE_CALLABLE_MATH_FLOOR:
    case XR_NATIVE_CALLABLE_MATH_CEIL:
    case XR_NATIVE_CALLABLE_MATH_ROUND:
    case XR_NATIVE_CALLABLE_MATH_TRUNC:
        return !identity->capture_owner && !identity->capture_count &&
            xir_native_callable_verify(c->source, index) == XR_XIR_OK ? identity->native_callable : 0;
    default:
        return 0;
    }
}

static bool lower_math_round_return(Lower *c, XrLirBlock block, XrLirValue result) {
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&result, .operand_count=1};
    return lower_build(c, xr_lir_set_terminator(c->module, block, &ret));
}

static bool lower_math_round_adjust(Lower *c, XrLirBlock block, XrLirValue integral, XrLirOp op) {
    XrLirValue operands[2] = {integral, {0}}, result = {0};
    XrLirInstruction one = {.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=1};
    XrLirInstruction adjust = {.op=op, .type=XR_LIR_I64, .operands=operands, .operand_count=2};
    return lower_build(c, xr_lir_emit_value(c->module, block, &one, &operands[1])) &&
        lower_build(c, xr_lir_emit_value(c->module, block, &adjust, &result)) &&
        lower_math_round_return(c, block, result);
}

static bool lower_math_round_branch(Lower *c, XrLirBlock block, XrLirValue fraction,
    uint64_t threshold, XrLirPredicate predicate, XrLirBlock adjust, XrLirBlock otherwise) {
    XrLirValue operands[2] = {fraction, {0}}, selected = {0};
    XrLirInstruction constant = {.op=XR_LIR_CONST, .type=XR_LIR_F64, .bits=threshold};
    XrLirInstruction compare = {.op=XR_LIR_FCMP, .type=XR_LIR_I1, .predicate=predicate,
        .operands=operands, .operand_count=2};
    XrLirInstruction branch = {.op=XR_LIR_CONDBR, .type=XR_LIR_UNIT, .operands=&selected,
        .operand_count=1, .edge_count=2, .edges={{.target=adjust}, {.target=otherwise}}};
    return lower_build(c, xr_lir_emit_value(c->module, block, &constant, &operands[1])) &&
        lower_build(c, xr_lir_emit_value(c->module, block, &compare, &selected)) &&
        lower_build(c, xr_lir_set_terminator(c->module, block, &branch));
}

static bool lower_math_rounding_body(Lower *c, XrLirFunction function, uint32_t id) {
    XrLirBlock entry = {0}, unchanged = {0}, adjust = {0};
    XrLirValue input = {0}, integral = {0}, back = {0}, fraction = {0};
    XrLirInstruction to_int = {.op=XR_LIR_FTOI, .type=XR_LIR_I64, .source=lower_source(c),
        .operands=&input, .operand_count=1};
    if (!lower_build(c, xr_lir_define_function(c->module, function)) ||
        !lower_build(c, xr_lir_add_block(c->module, function, NULL, 0, &entry)) ||
        !lower_build(c, xr_lir_set_entry(c->module, function, entry)) ||
        !lower_build(c, xr_lir_function_parameter(c->module, function, 0, &input)) ||
        !lower_build(c, xr_lir_emit_value(c->module, entry, &to_int, &integral))) return false;
    if (id == XR_NATIVE_CALLABLE_MATH_TRUNC)
        return lower_math_round_return(c, entry, integral);

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
        !lower_math_round_return(c, unchanged, integral)) return false;

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
