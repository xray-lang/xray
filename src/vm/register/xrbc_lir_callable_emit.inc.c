/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_callable_emit.inc.c - Typed function producers and invocation sites
 */
static bool rbc_lir_callable_emit(RbcLirCompiler *c, const XrLirInstructionView *instruction) {
    if (instruction->op == XR_LIR_FUNCTION_REF)
        return rbc_lir_aggregate_append(c, (XrRbcAggregateAccess){.operation=XR_RBC_FUNCTION_REFERENCE,
            .result={c->slots[instruction->result], false},
            .slot=rbc_lir_callable_ordinal(c, instruction->type) + 1,
            .callee=c->functions[instruction->callee - 1] - 1});
    uint32_t window = c->register_count - instruction->operand_count;
    for (uint32_t p = 1; p < instruction->operand_count; ++p) {
        if (rbc_lir_aggregate_type(c->input, rbc_lir_value_type(c, instruction->operands[p])))
            return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "indirect aggregate parameters require owned homes");
        if (!rbc_lir_emit(c, XR_RBC_MOV, window + p, c->slots[instruction->operands[p]], 0, 0)) return false;
    }
    XrLirType callable = rbc_lir_value_type(c, instruction->operands[0]);
    if (!rbc_lir_aggregate_append(c, (XrRbcAggregateAccess){.operation=XR_RBC_FUNCTION_CALL,
            .source=c->slots[instruction->operands[0]], .slot=rbc_lir_callable_ordinal(c, callable) + 1,
            .window=window})) return false;
    return !instruction->result || rbc_lir_array_move(c,
        rbc_lir_register_kind(c, instruction->result, instruction->type), c->slots[instruction->result], window);
}
