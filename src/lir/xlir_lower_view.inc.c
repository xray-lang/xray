/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_view.inc.c - Natural readonly windows on actual Array bindings
 */
static XrLirType lower_slice_element(const Lower *c, XrXirType type) {
    const XrXirTypeNode *node = xr_xir_type_node(c->source->types, type);
    return node && node->kind == XR_XIR_TYPE_SLICE ? lower_type(c, node->element) : XR_LIR_TYPE_INVALID;
}
static bool lower_slice_call(LowerFunction *f, uint32_t block, XrLirSliceOperation role,
    XrLirType element, const XrLirValue *args, uint32_t count, XrLirValue *output) {
    Lower *c = f->owner; uint32_t id = 0; XrLirType binding = XR_LIR_TYPE_INVALID;
    if (role == XR_LIR_SLICE_OP_NEW)
        binding = c->module->functions[f->function.id - 1].values[args[0].id - 1].type;
    if (!lower_build(c, xr_lir_slice_operation(c->module, role, element, binding, &id))) return false;
    const XrLirIntrinsic *entry = xr_lir_internal_slice_intrinsic(c->module, id);
    if (!entry) return lower_unsupported(c, "Slice role lost its receiving signature");
    XrLirType result = entry->result;
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC,.type=result,.intrinsic=id,
        .operands=args,.operand_count=count,.source=lower_source(c)};
    return lower_build(c, result == XR_LIR_UNIT ? xr_lir_emit_effect(c->module, f->blocks[block], &call) :
        xr_lir_emit_value(c->module, f->blocks[block], &call, output));
}
static bool lower_slice_source(LowerFunction *f, uint32_t ordinal, XrLirValue *output) {
    if (ordinal >= f->value_count) return lower_unsupported(f->owner, "Slice cleanup source ordinal");
    if (f->cell_for[ordinal] != UINT32_MAX &&
        lower_slice_element(f->owner, xr_xir_operand_type(f->source, ordinal))) {
        *output = f->current[f->cell_for[ordinal]];
        return output->id != 0 || lower_unsupported(f->owner, "Slice local has no live SSA binding");
    }
    return lower_value(f, ordinal, output);
}
static bool lower_view_empty_entry(LowerFunction *f, uint32_t block) {
    if (block || f->owner->instruction) return false;
    for (uint32_t b = 0; b < f->source->block_count; ++b) if (f->reachable[b]) {
        const XrXirInstruction *term = lower_term(f->owner, f->source, b);
        if (!term) return false;
        for (uint32_t e = 0; e < lower_edge_count(term); ++e)
            if (!term->targets[e]) return false;
    }
    /* No edge can revisit this activation entry. Slice formals are excluded,
     * so every local keeper is truly empty before its first instruction. */
    return true;
}
static bool lower_view_cleanup(LowerFunction *f, uint32_t block, const XrXirInstruction *op) {
    if (lower_view_empty_entry(f, block)) return true;
    for (uint32_t n = 0; n < op->args[1]; ++n) {
        uint32_t ordinal = f->source->operands[op->args[0] + n];
        if (ordinal >= f->value_count) return lower_unsupported(f->owner, "Slice cleanup source ordinal");
        if (ordinal >= f->source->parameter_count && f->value_blocks && f->value_blocks[ordinal] == UINT32_MAX) continue;
        XrLirType element = lower_slice_element(f->owner, xr_xir_operand_type(f->source, ordinal));
        XrLirValue value = {0};
        if (!element) return lower_unsupported(f->owner, "Slice cleanup requires a natural readonly Array loan");
        if (!lower_slice_source(f, ordinal, &value) ||
            !lower_slice_call(f, block, XR_LIR_SLICE_OP_END, element, &value, 1, NULL)) return false;
    }
    return true;
}
static bool lower_view_local(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    const XrXirInstruction *op = &f->source->instructions[index];
    bool create = op->op == XR_XIR_LOCAL_NEW, read = op->op == XR_XIR_LOCAL_READ, write = op->op == XR_XIR_LOCAL_WRITE;
    if (!create && !read && !write) return true;
    XrXirType type = write ? xr_xir_operand_type(f->source, op->args[0]) : op->type;
    XrLirType element = lower_slice_element(f->owner, type);
    if (!element) return true;
    *handled = true;
    uint32_t result = f->source->parameter_count + index, cell = 0;
    if (!lower_cell_index(f, create ? result : op->args[0], &cell)) return false;
    XrLirValue input = f->current[cell];
    if (!read && !lower_slice_source(f, op->args[create ? 0 : 1], &input)) return false;
    return lower_slice_call(f, block, XR_LIR_SLICE_OP_BIND, element, &input, 1,
        read ? &f->values[result] : &f->current[cell]);
}
static bool lower_view_instruction(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    Lower *c = f->owner; const XrXirInstruction *op = &f->source->instructions[index];
    uint32_t result = f->source->parameter_count + index;
    *handled = false;
    if (op->op == XR_XIR_VIEW_CLEANUP) { *handled = true; return lower_view_cleanup(f, block, op); }
    if (!lower_view_local(f, block, index, handled)) return false;
    if (*handled) return true;
    XrLirValue args[4] = {{0}}; uint32_t count = 1;
    XrLirSliceOperation role; XrLirType element;
    if (op->op == XR_XIR_VIEW_NEW || op->op == XR_XIR_VIEW_REBORROW) {
        element = lower_slice_element(c, op->type);
        for (uint32_t a = 0; a < 3; ++a)
            if (!lower_slice_source(f, f->source->operands[op->args[0] + a], &args[a])) return false;
        role = op->op == XR_XIR_VIEW_NEW ? XR_LIR_SLICE_OP_NEW : XR_LIR_SLICE_OP_REBORROW;
        count = 3;
        if (role == XR_LIR_SLICE_OP_REBORROW) {
            XrLirInstruction open = {.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=(uint64_t)op->immediate,.source=lower_source(c)};
            if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[block], &open, &args[count++]))) return false;
        } else if (op->immediate) return lower_unsupported(c, "readonly Array Slice requires normalized bounds");
    } else if (op->op == XR_XIR_VIEW_LEN || op->op == XR_XIR_VIEW_GET || op->op == XR_XIR_VIEW_COPY || op->op == XR_XIR_VIEW_END) {
        element = lower_slice_element(c, xr_xir_operand_type(f->source, op->args[0]));
        if (!lower_slice_source(f, op->args[0], &args[0])) return false;
        role = op->op == XR_XIR_VIEW_LEN ? XR_LIR_SLICE_OP_LEN : op->op == XR_XIR_VIEW_GET ? XR_LIR_SLICE_OP_GET :
            op->op == XR_XIR_VIEW_COPY ? XR_LIR_SLICE_OP_COPY : XR_LIR_SLICE_OP_END;
        if (op->op == XR_XIR_VIEW_GET) {
            if (!lower_value(f, op->args[1], &args[count++])) return false;
        }
    } else if (op->op == XR_XIR_COPY && (element = lower_slice_element(c, op->type)) != 0) {
        role = XR_LIR_SLICE_OP_BIND;
        if (!lower_slice_source(f, op->args[0], &args[0])) return false;
    } else return true;
    *handled = true;
    if (!element) return lower_unsupported(c, "readonly Array Slice source type");
    return lower_slice_call(f, block, role, element, args, count, &f->values[result]);
}
