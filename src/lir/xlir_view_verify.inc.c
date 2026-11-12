/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_view_verify.inc.c - Library calls below exact activation producers
 */
static bool lir_view_operation(XrLirOp op) {
    return op == XR_LIR_VIEW_FRAME_ENTER || op == XR_LIR_VIEW_FRAME_LEAVE;
}
static uint32_t lir_view_call_operation(const XrLirModule *m, const XrLirInstructionView *v) {
    const XrLirIntrinsic *entry = v->op == XR_LIR_CALL_INTRINSIC ?
        xr_lir_internal_view_intrinsic(m, v->intrinsic) : NULL;
    return entry ? entry->view_operation : 0;
}
static bool lir_view_point(const XrLirModule *m, const XrLirInstructionView *v) {
    return lir_view_operation(v->op) || lir_view_call_operation(m, v);
}
static const XrLirInstructionView *lir_view_frame_producer(const XrLirFunctionData *f, uint32_t value) {
    const XrLirInstructionView *producer = xr_lir_internal_value_definition(f, value);
    return producer && producer->op == XR_LIR_VIEW_FRAME_ENTER && producer->type == XR_LIR_VIEW_FRAME ?
        producer : NULL;
}
static bool lir_view_instruction_types(const XrLirModule *m,
    const XrLirFunctionData *f, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_VIEW_FRAME_ENTER)
        return v->type == XR_LIR_VIEW_FRAME && !v->operand_count;
    if (!v->operand_count || f->values[v->operands[0] - 1].type != XR_LIR_VIEW_FRAME) return false;
    const XrLirInstructionView *frame = lir_view_frame_producer(f, v->operands[0]);
    if (!frame) return false;
    if (v->op == XR_LIR_VIEW_FRAME_LEAVE) return v->type == XR_LIR_UNIT && !v->bits;
    uint32_t role = lir_view_call_operation(m, v);
    if (!role || v->bits) return false;
    for (uint32_t p = 0; p < v->operand_count; ++p) {
        const XrLirValueData *input = &f->values[v->operands[p] - 1];
        if (input->type == XR_LIR_ARRAY && input->array_element != XR_LIR_I64) return false;
    }
    bool cell_slot = role == XR_LIR_VIEW_OP_CELL_NEW || role == XR_LIR_VIEW_OP_CELL_IMPORT;
    bool view_slot = role == XR_LIR_VIEW_OP_NEW || role == XR_LIR_VIEW_OP_REBORROW ||
        role == XR_LIR_VIEW_OP_BIND || role == XR_LIR_VIEW_OP_CLEANUP;
    if (cell_slot || view_slot || role == XR_LIR_VIEW_OP_CELL_CONSTRUCT) {
        uint32_t parameter = role == XR_LIR_VIEW_OP_CELL_CONSTRUCT ? 3u : 1u;
        const XrLirInstructionView *slot = xr_lir_internal_value_definition(f, v->operands[parameter]);
        if (!slot || slot->op != XR_LIR_CONST || slot->type != XR_LIR_U32 || slot->bits > UINT32_MAX)
            return false;
        if (cell_slot && slot->bits >= (uint32_t)frame->bits) return false;
        if (view_slot && slot->bits >= (uint32_t)(frame->bits >> 32)) return false;
        if (role == XR_LIR_VIEW_OP_CELL_CONSTRUCT &&
            (slot->bits >= m->state_layout.slot_count ||
             m->state_layout.slots[slot->bits].payload != XR_LIR_VIEW_CELL ||
             !m->state_layout.slots[slot->bits].mutable)) return false;
    }
    if (role == XR_LIR_VIEW_OP_CELL_IMPORT &&
        f->values[v->operands[2] - 1].state_reference.payload != XR_LIR_VIEW_CELL) return false;
    if (role == XR_LIR_VIEW_OP_NEW || role == XR_LIR_VIEW_OP_REBORROW) {
        const XrLirInstructionView *open = xr_lir_internal_value_definition(f, v->operands[5]);
        if (!open || open->op != XR_LIR_CONST || open->type != XR_LIR_I1 || open->bits > 1) return false;
    }
    return true;
}
static bool lir_view_function_shape(const XrLirModule *m, const XrLirFunctionData *f) {
    if (f->result == XR_LIR_VIEW_FRAME || f->result == XR_LIR_VIEW_CELL) return false;
    bool needed = f->result == XR_LIR_SLICE_I64;
    for (uint32_t p = 0; p < f->parameter_count; ++p) {
        if (f->parameter_types[p] == XR_LIR_VIEW_FRAME) return false;
        needed |= f->parameter_types[p] == XR_LIR_SLICE_I64 || f->parameter_types[p] == XR_LIR_VIEW_CELL;
    }
    if (f->kind == XR_LIR_FUNCTION_EXTERNAL) return !needed;
    uint32_t frame = 0;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            needed |= lir_view_point(m, v);
            if (v->op == XR_LIR_VIEW_FRAME_ENTER) {
                if (frame || b + 1 != f->entry || i ||
                    (uint64_t)(uint32_t)v->bits + (v->bits >> 32) > f->value_count) return false;
                frame = v->result;
            }
            if (v->op == XR_LIR_VIEW_FRAME_LEAVE &&
                (i + 2 != block->instruction_count || block->instructions[i + 1].view.op != XR_LIR_RET)) return false;
        }
    }
    if (!needed) return true;
    if (!frame) return false;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrLirBlockData *block = &f->blocks[b];
        for (uint32_t i = 0; i < block->instruction_count; ++i) {
            const XrLirInstructionView *v = &block->instructions[i].view;
            if (lir_view_point(m, v) && v->op != XR_LIR_VIEW_FRAME_ENTER &&
                (!v->operand_count || v->operands[0] != frame)) return false;
            if (v->op == XR_LIR_RET && (!i || block->instructions[i - 1].view.op != XR_LIR_VIEW_FRAME_LEAVE))
                return false;
            if (v->op == XR_LIR_RET && f->result == XR_LIR_SLICE_I64) {
                const XrLirInstructionView *result = v->operand_count == 1 ?
                    xr_lir_internal_value_definition(f, v->operands[0]) : NULL;
                if (!result || lir_view_call_operation(m, result) != XR_LIR_VIEW_OP_RETURN) return false;
            }
        }
    }
    return true;
}
