/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_string_verify.inc.c - Synchronous String references preserve their binding
 */
static XrLirStatus lir_string_references(const XrLirModule *m, const XrLirFunctionData *f,
    uint32_t function, uint8_t *released, XrLirDiagnostic *d) {
    for (uint32_t id = 1; id <= f->value_count; ++id) {
        const XrLirValueData *value = &f->values[id - 1];
        if (value->type != XR_LIR_STRING_REF) continue;
        if (value->parameter && !value->block && function != m->state_entry) continue;
        const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
        uint32_t role = xr_lir_string_role(m, definition);
        if (role != XR_LIR_STRING_OP_BORROW_CELL && role != XR_LIR_STRING_OP_BORROW_STATE)
            return xr_lir_internal_error(d, "String reference requires an actual synchronous binding",
                function, value->block, 0, NULL);
    }
    for (uint32_t b = 0; b < f->block_count; ++b) {
        memset(released, 0, f->value_count);
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            for (uint32_t a = 0; a < v->operand_count; ++a) {
                uint32_t id = v->operands[a];
                if (f->values[id - 1].type != XR_LIR_STRING_REF) continue;
                uint32_t role = xr_lir_string_role(m, v);
                if (v->op != XR_LIR_CALL && role != XR_LIR_STRING_OP_READ_REF &&
                    role != XR_LIR_STRING_OP_WRITE_REF && role != XR_LIR_STRING_OP_APPEND_REF)
                    return xr_lir_internal_error(d, "String reference cannot escape its synchronous call",
                        function, b + 1, i + 1, &v->source);
                const XrLirInstructionView *definition = xr_lir_internal_value_definition(f, id);
                if (xr_lir_string_role(m, definition) == XR_LIR_STRING_OP_BORROW_CELL &&
                    released[definition->operands[0] - 1])
                    return xr_lir_internal_error(d, "String reference outlives its actual Cell",
                        function, b + 1, i + 1, &v->source);
            }
            if (v->op == XR_LIR_STRING_CELL_DROP) released[v->operands[0] - 1] = 1;
        }
        const XrLirInstructionView *term = lir_terminator(f, b);
        for (uint32_t e = 0; e < term->edge_count; ++e)
            for (uint32_t a = 0; a < term->edges[e].argument_count; ++a)
                if (f->values[term->edges[e].arguments[a] - 1].type == XR_LIR_STRING_REF)
                    return xr_lir_internal_error(d, "String reference cannot transfer on an edge",
                        function, b + 1, 0, &term->source);
    }
    return XR_LIR_OK;
}
