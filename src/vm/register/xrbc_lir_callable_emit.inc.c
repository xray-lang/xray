/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_callable_emit.inc.c - Typed function producers and invocation sites
 */
static bool rbc_lir_callable_emit(RbcLirCompiler *c, const XrLirInstructionView *instruction) {
    if (instruction->op == XR_LIR_FUNCTION_REF) {
        XrRbcAggregateAccess access = {.operation=XR_RBC_FUNCTION_REFERENCE,
            .result={c->slots[instruction->result], false},
            .slot=rbc_lir_callable_ordinal(c, instruction->type) + 1,
            .callee=c->functions[instruction->callee - 1] - 1};
        if (instruction->operand_count) {
            uint32_t count = instruction->operand_count;
            if (c->environment_count == UINT32_MAX || count > (UINT32_MAX - c->environment_size) / 8u ||
                c->aggregate_operand_count > UINT32_MAX - count)
                return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "frame captures exceed indexed capacity");
            void *storage = c->environments;
            if (!rbc_lir_grow(c, &storage, &c->environment_capacity,
                c->environment_count + 1, sizeof(*c->environments))) return false;
            c->environments = storage;
            storage = c->aggregate_operands;
            if (!rbc_lir_grow(c, &storage, &c->aggregate_operand_capacity,
                c->aggregate_operand_count + count, sizeof(*c->aggregate_operands))) return false;
            c->aggregate_operands = storage;
            access.destination = (uint32_t)c->environment_count + 1;
            access.first = (uint32_t)c->aggregate_operand_count;
            access.count = count;
            c->environments[c->environment_count++] = (XrRbcEnvironment){access.callee,
                c->environment_size, (uint32_t)c->aggregate_access_count};
            c->environment_size += count * 8u;
            for (uint32_t p = 0; p < count; ++p)
                c->aggregate_operands[c->aggregate_operand_count++] = rbc_lir_array_location(c, instruction->operands[p]);
        }
        return rbc_lir_aggregate_append(c, access);
    }
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
