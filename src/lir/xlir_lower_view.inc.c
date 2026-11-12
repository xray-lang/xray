/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_view.inc.c - Actual activation slots below Checked view bindings
 */
typedef struct LowerViewCall {
    XrLirViewOperation operation;
    const XrLirValue *operands;
    uint32_t count;
    uint64_t bits;
} LowerViewCall;

static bool lower_view_emit(LowerFunction *f, uint32_t block,
    XrLirInstruction instruction, XrLirValue *output) {
    instruction.source = lower_source(f->owner);
    return lower_build(f->owner, instruction.type == XR_LIR_UNIT ?
        xr_lir_emit_effect(f->owner->module, f->blocks[block], &instruction) :
        xr_lir_emit_value(f->owner->module, f->blocks[block], &instruction, output));
}
static bool lower_view_constant(LowerFunction *f, uint32_t block,
    XrLirType type, uint64_t bits, XrLirValue *output) {
    XrLirInstruction constant = {.op=XR_LIR_CONST, .type=type, .bits=bits};
    return lower_view_emit(f, block, constant, output);
}
static bool lower_view_call(LowerFunction *f, uint32_t block,
    const LowerViewCall *call, XrLirValue *output) {
    Lower *c = f->owner;
    uint32_t id = 0;
    if (!lower_build(c, xr_lir_view_operation(c->module, call->operation, &id))) return false;
    const XrLirIntrinsic *entry = xr_lir_internal_view_intrinsic(c->module, id);
    XR_CHECK(entry && call->count && call->count <= 4, "view lowering consumes the installed closed role catalog");
    bool slot = call->operation == XR_LIR_VIEW_OP_CELL_NEW || call->operation == XR_LIR_VIEW_OP_CELL_IMPORT ||
        call->operation == XR_LIR_VIEW_OP_NEW || call->operation == XR_LIR_VIEW_OP_REBORROW ||
        call->operation == XR_LIR_VIEW_OP_BIND || call->operation == XR_LIR_VIEW_OP_CLEANUP;
    bool range = call->operation == XR_LIR_VIEW_OP_NEW || call->operation == XR_LIR_VIEW_OP_REBORROW;
    XrLirValue args[6] = {{0}};
    uint32_t count = 0;
    args[count++] = call->operands[0];
    if (slot && !lower_view_constant(f, block, XR_LIR_U32, (uint32_t)call->bits, &args[count++])) return false;
    for (uint32_t n = 1; n < call->count; ++n) args[count++] = call->operands[n];
    if (call->operation == XR_LIR_VIEW_OP_CELL_CONSTRUCT &&
        !lower_view_constant(f, block, XR_LIR_U32, call->bits, &args[count++])) return false;
    if (range && !lower_view_constant(f, block, XR_LIR_I1, call->bits >> 32, &args[count++])) return false;
    XrLirInstruction instruction = {.op=XR_LIR_CALL_INTRINSIC, .type=entry->result,
        .intrinsic=id, .operands=args, .operand_count=count};
    return lower_view_emit(f, block, instruction, output);
}
static bool lower_view_bind(LowerFunction *f, uint32_t block, uint32_t slot,
    XrLirValue value, XrLirValue *output) {
    XrLirValue args[] = {f->view_frame, value};
    LowerViewCall call = {XR_LIR_VIEW_OP_BIND, args, 2, slot};
    return lower_view_call(f, block, &call, output);
}
static bool lower_view_inventory(LowerFunction *f) {
    Lower *c = f->owner;
    if (!c->views) return true;
    f->view_slots = lower_alloc(c, f->value_count, sizeof(*f->view_slots));
    f->view_cells = lower_alloc(c, f->value_count, sizeof(*f->view_cells));
    f->view_module_refs = lower_alloc(c, f->value_count, sizeof(*f->view_module_refs));
    if (f->value_count && (!f->view_slots || !f->view_cells || !f->view_module_refs)) return false;
    for (uint32_t v = 0; v < f->value_count; ++v) f->view_slots[v] = f->view_cells[v] = UINT32_MAX;
    for (uint32_t p = 0; p < f->source->parameter_count; ++p) {
        XrLirType type = lower_type(c, f->source->parameters[p]);
        if (type == XR_LIR_SLICE_I64) f->view_slots[p] = f->view_slot_count++;
        f->view_needed |= type == XR_LIR_SLICE_I64 || type == XR_LIR_VIEW_CELL;
    }
    for (uint32_t b = 0; b < f->source->block_count; ++b) if (f->reachable[b]) {
        const XrXirBlock *block = &f->source->blocks[b];
        for (uint32_t i = block->first; i < block->first + block->count; ++i) {
            const XrXirInstruction *op = &f->source->instructions[i];
            uint32_t value = f->source->parameter_count + i;
            XrLirType type = lower_type(c, op->type);
            bool physical_cell = type == XR_LIR_VIEW_CELL &&
                !(f->promoted_arrays && f->promoted_arrays[value]);
            if (type == XR_LIR_SLICE_I64) f->view_slots[value] = f->view_slot_count++;
            if (physical_cell && (op->op == XR_XIR_CELL_NEW ||
                op->op == XR_XIR_SLOT_LOAD || op->op == XR_XIR_SLOT_PLACE)) f->view_cells[value] = f->view_cell_count++;
            f->view_needed |= type == XR_LIR_SLICE_I64 || physical_cell ||
                (op->op == XR_XIR_VIEW_CLEANUP && op->args[1]);
        }
    }
    return true;
}
static bool lower_view_enter(LowerFunction *f) {
    if (!f->view_needed) return true;
    XrLirInstruction enter = {.op=XR_LIR_VIEW_FRAME_ENTER, .type=XR_LIR_VIEW_FRAME,
        .bits=f->view_cell_count | ((uint64_t)f->view_slot_count << 32)};
    if (!lower_view_emit(f, 0, enter, &f->view_frame)) return false;
    for (uint32_t p = 0; p < f->source->parameter_count; ++p)
        if (f->view_slots[p] != UINT32_MAX &&
            !lower_view_bind(f, 0, f->view_slots[p], f->values[p], &f->values[p])) return false;
    return true;
}
static bool lower_view_cleanup(LowerFunction *f, uint32_t block, const XrXirInstruction *op) {
    if (!f->view_needed) return op->args[1] == 0;
    for (uint32_t n = 0; n < op->args[1]; ++n) {
        uint32_t owner = f->source->operands[op->args[0] + n];
        if (owner >= f->value_count) return lower_unsupported(f->owner, "view cleanup owner ordinal");
        uint32_t slot = f->view_slots[owner];
        if (slot == UINT32_MAX) {
            if (owner < f->source->parameter_count || !f->value_blocks || f->value_blocks[owner] != UINT32_MAX)
                return lower_unsupported(f->owner, "view cleanup owner type");
            continue;
        }
        LowerViewCall call = {XR_LIR_VIEW_OP_CLEANUP, &f->view_frame, 1, slot};
        if (!lower_view_call(f, block, &call, NULL)) return false;
    }
    return true;
}
static bool lower_view_local(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    const XrXirInstruction *op = &f->source->instructions[index];
    uint32_t result = f->source->parameter_count + index;
    bool create = op->op == XR_XIR_LOCAL_NEW, read = op->op == XR_XIR_LOCAL_READ;
    bool write = op->op == XR_XIR_LOCAL_WRITE;
    XrXirType type = write ? xr_xir_operand_type(f->source, op->args[0]) : op->type;
    if ((!create && !read && !write) || lower_type(f->owner, type) != XR_LIR_SLICE_I64) return true;
    *handled = true;
    uint32_t place = create ? result : op->args[0], cell;
    if (!lower_cell_index(f, place, &cell)) return false;
    XrLirValue input = f->current[cell];
    if (!read && !lower_value(f, op->args[create ? 0 : 1], &input)) return false;
    return lower_view_bind(f, block, f->view_slots[read ? result : place], input,
        read ? &f->values[result] : &f->current[cell]);
}

#include "xlir_lower_view_cells.inc.c"

static bool lower_view_instruction(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    const XrXirInstruction *op = &f->source->instructions[index];
    uint32_t result = f->source->parameter_count + index;
    *handled = false;
    if (op->op == XR_XIR_VIEW_CLEANUP) { *handled = true; return lower_view_cleanup(f, block, op); }
    if (!f->view_needed) return true;
    if (!lower_view_local(f, block, index, handled)) return false;
    if (*handled) return true;
    if (!lower_view_module(f, block, index, handled)) return false;
    if (*handled) return true;
    if (!lower_view_cell(f, block, index, handled)) return false;
    if (*handled) return true;
    if (!lower_view_mutation(f, block, index, handled)) return false;
    if (*handled) return true;
    XrLirValue args[4] = {f->view_frame, {0}, {0}, {0}};
    LowerViewCall call = {.operands=args};
    if (op->op == XR_XIR_VIEW_NEW || op->op == XR_XIR_VIEW_REBORROW) {
        for (uint32_t n = 0; n < 3; ++n)
            if (!lower_value(f, f->source->operands[op->args[0] + n], &args[n + 1])) return false;
        call.operation = op->op == XR_XIR_VIEW_NEW ? XR_LIR_VIEW_OP_NEW : XR_LIR_VIEW_OP_REBORROW;
        call.count = 4;
        call.bits = f->view_slots[result] | ((uint64_t)op->immediate << 32);
    } else if (op->op == XR_XIR_VIEW_LEN || op->op == XR_XIR_VIEW_GET ||
        op->op == XR_XIR_VIEW_COPY || op->op == XR_XIR_VIEW_END) {
        if (!lower_value(f, op->args[0], &args[1])) return false;
        call.operation = op->op == XR_XIR_VIEW_LEN ? XR_LIR_VIEW_OP_LEN :
            op->op == XR_XIR_VIEW_GET ? XR_LIR_VIEW_OP_GET :
            op->op == XR_XIR_VIEW_COPY ? XR_LIR_VIEW_OP_COPY : XR_LIR_VIEW_OP_END;
        call.count = op->op == XR_XIR_VIEW_GET ? 3u : 2u;
        if (op->op == XR_XIR_VIEW_GET && !lower_value(f, op->args[1], &args[2])) return false;
    } else if (op->op == XR_XIR_COPY && lower_type(f->owner, op->type) == XR_LIR_SLICE_I64) {
        if (!lower_value(f, op->args[0], &args[1])) return false;
        *handled = true;
        return lower_view_bind(f, block, f->view_slots[result], args[1], &f->values[result]);
    } else return true;
    *handled = true;
    return lower_view_call(f, block, &call, &f->values[result]);
}
static bool lower_view_phis(LowerFunction *f, uint32_t block) {
    if (!f->view_needed) return true;
    uint32_t first = f->source->parameter_count + f->source->blocks[block].first;
    for (uint32_t p = 0; p < f->phis[block]; ++p)
        if (f->view_slots[first + p] != UINT32_MAX && !lower_view_bind(f, block,
            f->view_slots[first + p], f->values[first + p], &f->values[first + p])) return false;
    return true;
}
static bool lower_view_leave(LowerFunction *f, uint32_t block, XrLirValue *result) {
    if (!f->view_needed) return true;
    if (lower_type(f->owner, f->source->result) == XR_LIR_SLICE_I64) {
        XrLirValue args[] = {f->view_frame, *result};
        LowerViewCall call = {XR_LIR_VIEW_OP_RETURN, args, 2, 0};
        if (!lower_view_call(f, block, &call, result)) return false;
    }
    XrLirInstruction leave = {.op=XR_LIR_VIEW_FRAME_LEAVE, .type=XR_LIR_UNIT,
        .operands=&f->view_frame, .operand_count=1};
    return lower_view_emit(f, block, leave, NULL);
}
static bool lower_view_state_layout(Lower *c) {
    const XrXirDeclarations *declarations = c->source->declarations;
    XrLirStateSlot *slots = (XrLirStateSlot *)c->state_layout.slots;
    for (uint32_t s = 0; s < declarations->slot_count; ++s) {
        uint32_t physical = c->state_slots[s];
        if (physical == UINT32_MAX || !declarations->slots[s].mutable ||
            lower_type(c, declarations->slots[s].type) != XR_LIR_VIEW_CELL) continue;
        slots[physical].payload = XR_LIR_VIEW_CELL;
        slots[physical].array_element = XR_LIR_TYPE_INVALID;
        slots[physical].size = 16;
        slots[physical].alignment = 8;
    }
    uint64_t end = 0;
    uint32_t alignment = 1;
    for (uint32_t s = 0; s < c->state_layout.slot_count; ++s) {
        XrLirStateSlot *slot = &slots[s];
        uint64_t padding = (slot->alignment - end % slot->alignment) % slot->alignment;
        if (end > UINT64_MAX - padding || end + padding > UINT64_MAX - slot->size)
            return lower_unsupported(c, "module view Cell payload size overflow");
        slot->offset = end + padding;
        end = slot->offset + slot->size;
        if (slot->alignment > alignment) alignment = slot->alignment;
    }
    uint64_t padding = (alignment - end % alignment) % alignment;
    if (end > UINT64_MAX - padding) return lower_unsupported(c, "module view Cell final alignment overflow");
    c->state_layout.payload_size = end + padding;
    c->state_layout.payload_alignment = alignment;
    return true;
}
