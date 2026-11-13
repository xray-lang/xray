/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_map.inc.c - Map values and actual bindings in the existing SSA graph
 *
 * KEY CONCEPT:
 *   The existing managed flow owns snapshots and selected-edge transfers.
 *   Mutations read the real binding after argument evaluation.
 */
static bool lower_map_read(LowerFunction *f, uint32_t block, XrLirValue receiver, XrLirValue *output) {
    XrLirType type = lower_value_type(f, receiver);
    if (type == XR_LIR_MAP_STRING_I64) { *output = receiver; return true; }
    if (type != XR_LIR_MAP_STRING_I64_CELL && type != XR_LIR_MAP_STRING_I64_REF && type != XR_LIR_STATE_REF)
        return lower_unsupported(f->owner, "Map receiver without a typed binding");
    XrLirInstruction read = {.op=type == XR_LIR_MAP_STRING_I64_CELL ? XR_LIR_CELL_READ :
        type == XR_LIR_MAP_STRING_I64_REF ? XR_LIR_MAP_REF_READ : XR_LIR_STATE_READ,
        .type=XR_LIR_MAP_STRING_I64, .operands=&receiver, .operand_count=1, .source=lower_source(f->owner)};
    return lower_build(f->owner, xr_lir_emit_value(f->owner->module, f->blocks[block], &read, output));
}
static bool lower_map_instruction(LowerFunction *f, uint32_t block, uint32_t index, bool *handled) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[index];
    uint32_t result = f->source->parameter_count + index;
    *handled = false;
    if (op->op == XR_XIR_CELL_PLACE || op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_PLACE_WRITE) {
        XrXirType type = op->op == XR_XIR_PLACE_WRITE ? xr_xir_operand_type(f->source, op->args[0]) : op->type;
        if (!lower_map_type(c, type)) return true;
        *handled = true;
        if (op->op == XR_XIR_CELL_PLACE) {
            if (f->array_places && f->array_places[result] != UINT32_MAX) return true;
            return lower_value(f, op->args[0], &f->values[result]);
        }
        uint32_t cell = f->array_places ? f->array_places[op->args[0]] : UINT32_MAX;
        if (cell != UINT32_MAX) {
            if (op->op == XR_XIR_PLACE_READ) return lower_managed_copy(f, block, f->current[cell], &f->values[result]);
            XrLirValue input = {0};
            return lower_value(f, op->args[1], &input) && lower_managed_copy(f, block, input, &f->current[cell]);
        }
        XrLirValue args[2] = {{0}};
        if (!lower_value(f, op->args[0], &args[0])) return false;
        if (op->op == XR_XIR_PLACE_READ) return lower_map_read(f, block, args[0], &f->values[result]);
        if (!lower_value(f, op->args[1], &args[1])) return false;
        XrLirType receiver = lower_value_type(f, args[0]);
        XrLirInstruction write = {.op=receiver == XR_LIR_STATE_REF ? XR_LIR_STATE_WRITE :
            receiver == XR_LIR_MAP_STRING_I64_REF ? XR_LIR_MAP_REF_WRITE : XR_LIR_CELL_WRITE,
            .type=XR_LIR_UNIT, .operands=args, .operand_count=2, .source=lower_source(c)};
        return lower_build(c, xr_lir_emit_effect(c->module, f->blocks[block], &write));
    }
    XrLirMapOperation operation;
    switch (op->op) {
    case XR_XIR_MAP_NEW: operation = XR_LIR_MAP_OP_NEW; break;
    case XR_XIR_MAP_GET: operation = XR_LIR_MAP_OP_GET; break;
    case XR_XIR_MAP_SET: operation = XR_LIR_MAP_OP_SET; break;
    case XR_XIR_HASH_LEN: operation = XR_LIR_MAP_OP_LEN; break;
    case XR_XIR_HASH_CONTAINS: operation = XR_LIR_MAP_OP_CONTAINS; break;
    case XR_XIR_HASH_DELETE: operation = XR_LIR_MAP_OP_REMOVE; break;
    case XR_XIR_HASH_CLEAR: operation = XR_LIR_MAP_OP_CLEAR; break;
    default: return true;
    }
    *handled = true;
    bool construct = operation == XR_LIR_MAP_OP_NEW;
    bool mutation = operation >= XR_LIR_MAP_OP_SET;
    bool grouped = op->op == XR_XIR_MAP_NEW || op->op == XR_XIR_MAP_SET;
    uint32_t count = grouped ? op->args[1] : operation == XR_LIR_MAP_OP_LEN || operation == XR_LIR_MAP_OP_CLEAR ? 1u : 2u;
    uint32_t source = construct ? UINT32_MAX : lower_operand_at(f->source, op, 0);
    XrXirType type = construct ? op->type : xr_xir_operand_type(f->source, source);
    if (!lower_map_type(c, type)) return lower_unsupported(c, "Map operation requires the String/I64 physical family");
    XrLirValue *args = lower_alloc(c, count, sizeof(*args));
    if (count && !args) return false;
    uint32_t cell = !construct && f->array_places ? f->array_places[source] : UINT32_MAX;
    bool good = true;
    for (uint32_t n = 0; n < count && good; ++n)
        if (!n && cell != UINT32_MAX) args[n] = f->current[cell];
        else good = lower_value(f, grouped ? f->source->operands[op->args[0] + n] : op->args[n], &args[n]);
    if (good && !construct) {
        XrLirType receiver = lower_value_type(f, args[0]);
        if (!mutation) good = lower_map_read(f, block, args[0], &args[0]);
        else if (receiver == XR_LIR_STATE_REF) good = lower_map_borrow(f, block, &args[0]);
        else if (cell != UINT32_MAX) {
            XrLirValue current = {0};
            good = lower_map_read(f, block, args[0], &current);
            XrLirInstruction create = {.op=XR_LIR_CELL_NEW, .type=XR_LIR_MAP_STRING_I64_CELL,
                .operands=&current, .operand_count=1, .source=lower_source(c)};
            if (good) good = lower_build(c, xr_lir_emit_value(c->module, f->blocks[block], &create, &args[0]));
            if (good && receiver == XR_LIR_MAP_STRING_I64) {
                XrLirInstruction drop = {.op=XR_LIR_MAP_DROP, .type=XR_LIR_UNIT,
                    .operands=&current, .operand_count=1, .source=lower_source(c)};
                good = lower_build(c, xr_lir_emit_effect(c->module, f->blocks[block], &drop));
            }
        } else if (receiver != XR_LIR_MAP_STRING_I64_CELL && receiver != XR_LIR_MAP_STRING_I64_REF)
            good = lower_unsupported(c, "Map mutation requires its actual mutable binding");
    }
    uint32_t intrinsic = 0;
    if (good) good = lower_build(c, xr_lir_map_operation(c->module, operation,
        construct ? XR_LIR_TYPE_INVALID : lower_value_type(f, args[0]), &intrinsic));
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC, .type=lower_type(c, op->type),
        .intrinsic=intrinsic, .operands=args, .operand_count=count, .source=lower_source(c)};
    if (good) good = lower_build(c, call.type == XR_LIR_UNIT ?
        xr_lir_emit_effect(c->module, f->blocks[block], &call) :
        xr_lir_emit_value(c->module, f->blocks[block], &call, &f->values[result]));
    if (good && mutation && cell != UINT32_MAX) {
        good = lower_map_read(f, block, args[0], &f->current[cell]);
        XrLirInstruction drop = {.op=XR_LIR_MAP_CELL_DROP, .type=XR_LIR_UNIT,
            .operands=args, .operand_count=1, .source=lower_source(c)};
        if (good) good = lower_build(c, xr_lir_emit_effect(c->module, f->blocks[block], &drop));
    }
    xr_free(args);
    return good;
}
