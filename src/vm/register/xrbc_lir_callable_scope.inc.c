/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_callable_scope.inc.c - Scope capabilities project into owned indices
 */
static bool rbc_lir_callable_scope_point(const XrLirInstructionView *instruction) {
    return instruction->op == XR_LIR_FUNCTION_SCOPE_ENTER || instruction->op == XR_LIR_FUNCTION_SCOPE_LEAVE ||
        instruction->op == XR_LIR_FUNCTION_BIND;
}
static bool rbc_lir_callable_scope_homes(RbcLirCompiler *c) {
    if (!c->view.scope_count) return true;
    XrRbcModule *module = &c->literals->program->module;
    if (!module->function_scopes) {
        if (module->function_count > SIZE_MAX / sizeof(XrRbcFunctionScopes))
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "scope directory exceeds address space");
        module->function_scopes = xr_calloc(module->function_count, sizeof(XrRbcFunctionScopes));
        if (!module->function_scopes)
            return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own function scope directory");
        module->function_scope_count = module->function_count;
    }
    XrRbcFunctionScopes *received = &((XrRbcFunctionScopes *)module->function_scopes)[c->functions[c->function.id - 1] - 1];
    uint64_t values = (uint64_t)c->view.value_count + 1;
    if (values > SIZE_MAX / sizeof(*c->scope_home) ||
        sizeof(uint32_t) > SIZE_MAX / c->view.scope_count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "scope homes exceed address space");
    c->scope_home = xr_malloc((size_t)values * sizeof(*c->scope_home));
    received->parents = xr_malloc((size_t)c->view.scope_count * sizeof(*received->parents));
    if (!c->scope_home || !received->parents)
        return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own indexed scope homes");
    received->count = c->view.scope_count;
    for (size_t v = 0; v < values; ++v) c->scope_home[v] = UINT32_MAX;
    uint32_t count = 0;
    for (uint32_t v = 1; v <= c->view.value_count; ++v) {
        XrLirType type = XR_LIR_TYPE_INVALID;
        xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, v}, &type);
        if (type == XR_LIR_FUNCTION_SCOPE) c->scope_home[v] = ++count;
    }
    XR_DCHECK(count == c->view.scope_count, "scope counts come from actual verified producers");
    for (uint32_t b = 0; b < c->view.block_count; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->input, c->function, b, &block);
        xr_lir_block_view(c->input, block, &view);
        for (uint32_t n = 0; n < view.instruction_count; ++n) {
            XrLirInstructionView instruction;
            xr_lir_instruction_view(c->input, block, n, &instruction);
            if (instruction.op == XR_LIR_FUNCTION_SCOPE_ENTER)
                ((uint32_t *)received->parents)[c->scope_home[instruction.result] - 1] =
                    instruction.operand_count ? c->scope_home[instruction.operands[0]] : 0;
        }
    }
    return true;
}
static bool rbc_lir_callable_scope_emit(RbcLirCompiler *c, const XrLirInstructionView *instruction) {
    XrRbcAggregateAccess access = {0};
    if (instruction->op == XR_LIR_FUNCTION_SCOPE_ENTER) {
        access.operation = XR_RBC_FUNCTION_SCOPE_ENTER;
        access.destination = c->scope_home[instruction->result];
        access.source = instruction->operand_count ? c->scope_home[instruction->operands[0]] : 0;
    } else {
        access.destination = c->scope_home[instruction->operands[0]];
        access.operation = instruction->op == XR_LIR_FUNCTION_BIND ? XR_RBC_FUNCTION_BIND : XR_RBC_FUNCTION_SCOPE_LEAVE;
        if (instruction->result) {
            access.source = c->slots[instruction->operands[1]];
            access.result.index = c->slots[instruction->result];
            if (rbc_lir_captured_cell(c->input, instruction->type)) {
                access.operation = XR_RBC_CAPTURED_CELL_BIND;
                access.slot = XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT + instruction->type - XR_LIR_TYPE_COUNT;
            } else {
                access.slot = rbc_lir_callable_ordinal(c, instruction->type) + 1;
                access.count = 1;
            }
        }
        if (instruction->op == XR_LIR_FUNCTION_BIND) access.window = (uint32_t)instruction->bits;
    }
    return rbc_lir_aggregate_append(c, access);
}
