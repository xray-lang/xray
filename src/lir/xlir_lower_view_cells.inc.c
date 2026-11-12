/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_view_cells.inc.c - Actual Cell and module State binding transport
 */
static bool lower_view_module(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[index];
    uint32_t result = f->source->parameter_count + index;
    if (op->op != XR_XIR_SLOT_LOAD && op->op != XR_XIR_SLOT_PLACE && op->op != XR_XIR_SLOT_INIT) return true;
    if (lower_type(c, c->source->declarations->slots[op->immediate].type) != XR_LIR_VIEW_CELL) return true;
    *handled = true;
    if (op->op == XR_XIR_SLOT_INIT) {
        XrLirValue reference = f->view_module_refs[op->args[0]];
        if (!reference.id) return lower_unsupported(c, "module Cell lacks its actual construction reference");
        XrLirInstruction publish = {.op=XR_LIR_STATE_PUBLISH, .type=XR_LIR_UNIT,
            .operands=&reference, .operand_count=1};
        return lower_view_emit(f, block, publish, NULL);
    }
    XrLirInstruction reference = {.op=XR_LIR_STATE_REFERENCE, .type=XR_LIR_STATE_REF,
        .operands=&f->state, .operand_count=1, .bits=c->state_slots[op->immediate]};
    if (!lower_view_emit(f, block, reference, &f->view_module_refs[result])) return false;
    XrLirValue args[] = {f->view_frame, f->view_module_refs[result]};
    LowerViewCall call = {XR_LIR_VIEW_OP_CELL_IMPORT, args, 2, f->view_cells[result]};
    return lower_view_call(f, block, &call, &f->values[result]);
}
static bool lower_view_cell(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[index];
    uint32_t result = f->source->parameter_count + index;
    bool create = op->op == XR_XIR_CELL_NEW;
    bool read = op->op == XR_XIR_CELL_READ || op->op == XR_XIR_PLACE_READ;
    bool write = op->op == XR_XIR_CELL_WRITE || op->op == XR_XIR_PLACE_WRITE;
    bool alias = op->op == XR_XIR_COPY || op->op == XR_XIR_CELL_PLACE;
    if (!create && !read && !write && !alias) return true;
    if (create && (lower_type(c, op->type) != XR_LIR_VIEW_CELL ||
        (f->promoted_arrays && f->promoted_arrays[result]))) return true;
    if (!create && !f->values[op->args[0]].id) return true;
    XrLirValue args[3] = {f->view_frame, {0}, {0}};
    if (!lower_value(f, op->args[0], &args[1])) return false;
    XrLirType input = c->module->functions[f->function.id - 1].values[args[1].id - 1].type;
    if (!create && input != XR_LIR_VIEW_CELL) return true;
    *handled = true;
    if (alias) {
        f->values[result] = args[1];
        f->view_module_refs[result] = f->view_module_refs[op->args[0]];
        return true;
    }
    LowerViewCall call = {.operation=create ? XR_LIR_VIEW_OP_CELL_NEW :
        read ? XR_LIR_VIEW_OP_CELL_READ : XR_LIR_VIEW_OP_CELL_WRITE,
        .operands=args, .count=write ? 3u : 2u, .bits=create ? f->view_cells[result] : 0};
    if (write && !lower_value(f, op->args[1], &args[2])) return false;
    if (!lower_view_call(f, block, &call, &f->values[result])) return false;
    if (!create) return true;
    uint32_t slot = lower_module_cell(c, c->function, result);
    if (slot == UINT32_MAX) return true;
    XrLirValue publish_args[] = {f->view_frame, f->values[result], f->state};
    LowerViewCall construct = {XR_LIR_VIEW_OP_CELL_CONSTRUCT, publish_args, 3, c->state_slots[slot]};
    return lower_view_call(f, block, &construct, &f->view_module_refs[result]);
}
static bool lower_view_mutation(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    const XrXirInstruction *op = &f->source->instructions[index];
    bool set = op->op == XR_XIR_ARRAY_SET;
    if (!set && op->op != XR_XIR_ARRAY_PUSH) return true;
    XrLirValue args[4] = {f->view_frame, {0}, {0}, {0}};
    uint32_t count = set ? op->args[1] : 2u;
    uint32_t receiver = set ? f->source->operands[op->args[0]] : op->args[0];
    if (!f->values[receiver].id) return true;
    if (!lower_value(f, receiver, &args[1])) return false;
    if (f->owner->module->functions[f->function.id - 1].values[args[1].id - 1].type != XR_LIR_VIEW_CELL) return true;
    *handled = true;
    if (count != (set ? 3u : 2u)) return lower_unsupported(f->owner, "integer Cell mutator shape");
    for (uint32_t a = 1; a < count; ++a)
        if (!lower_value(f, set ? f->source->operands[op->args[0] + a] : op->args[a], &args[a + 1])) return false;
    LowerViewCall call = {set ? XR_LIR_VIEW_OP_CELL_SET : XR_LIR_VIEW_OP_CELL_PUSH, args, count + 1, 0};
    return lower_view_call(f, block, &call, NULL);
}
