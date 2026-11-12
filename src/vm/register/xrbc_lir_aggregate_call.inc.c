/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_aggregate_call.inc.c - Typed calls with scalar windows and value homes
 */
static bool rbc_lir_aggregate_call_emit(RbcLirCompiler *c, const XrLirInstructionView *instruction) {
    uint32_t scalars = rbc_lir_scalar_arguments(c->input, instruction);
    uint32_t fixed = instruction->operand_count - scalars;
    XrRbcAggregateAccess access = {.operation=XR_RBC_AGGREGATE_CALL,
        .callee=c->functions[instruction->callee - 1] - 1, .window=c->register_count - scalars - 1,
        .first=(uint32_t)c->aggregate_argument_count, .count=fixed};
    if (rbc_lir_aggregate_type(c->input, instruction->type)) access.destination = c->aggregate_home[instruction->result];
    if (c->aggregate_argument_count > UINT32_MAX - fixed)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "aggregate call arguments exceed bytecode indices");
    if (fixed) {
        void *storage = c->aggregate_arguments;
        if (!rbc_lir_grow(c, &storage, &c->aggregate_argument_capacity,
            c->aggregate_argument_count + fixed, sizeof(*c->aggregate_arguments))) return false;
        c->aggregate_arguments = storage;
    }
    uint32_t physical = 0;
    for (uint32_t p = 0; p < instruction->operand_count; ++p) {
        uint32_t value = instruction->operands[p];
        if (c->aggregate_home && c->aggregate_home[value] != UINT32_MAX)
            c->aggregate_arguments[c->aggregate_argument_count++] = c->aggregate_home[value];
        else if (!rbc_lir_emit(c, XR_RBC_MOV, access.window + ++physical, c->slots[value], 0, 0)) return false;
    }
    if (!rbc_lir_aggregate_append(c, access)) return false;
    if (!instruction->result || rbc_lir_aggregate_type(c->input, instruction->type)) return true;
    return rbc_lir_array_move(c, rbc_lir_register_kind(c, instruction->result, instruction->type),
        c->slots[instruction->result], access.window);
}
