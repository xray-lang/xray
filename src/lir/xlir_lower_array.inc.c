/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_array.inc.c - Closed Array values and binding-preserving mutations
 *
 * KEY CONCEPT:
 *   A place keeps its real binding through argument evaluation. Module writes
 *   use a temporary Cell only at the mutation point, then replace that binding.
 */
static XrLirType lower_value_type(const LowerFunction *f, XrLirValue value) {
    return f->owner->module->functions[f->function.id - 1].values[value.id - 1].type;
}

static bool lower_array_read(LowerFunction *f, uint32_t b, XrLirValue receiver,
    XrLirValue *output) {
    XrLirType type = lower_value_type(f, receiver);
    if (type == XR_LIR_ARRAY) { *output = receiver; return true; }
    if (type == XR_LIR_VIEW_CELL) {
        XrLirValue args[] = {f->view_frame, receiver};
        LowerViewCall call = {XR_LIR_VIEW_OP_CELL_READ, args, 2, 0};
        return lower_view_call(f, b, &call, output);
    }
    if (type != XR_LIR_ARRAY_CELL && type != XR_LIR_STATE_REF && type != XR_LIR_ARRAY_REF)
        return lower_unsupported(f->owner, "Array receiver without a typed binding");
    XrLirInstruction read = {.op = type == XR_LIR_ARRAY_CELL ? XR_LIR_CELL_READ :
        type == XR_LIR_ARRAY_REF ? XR_LIR_ARRAY_REF_READ : XR_LIR_STATE_READ,
        .type = XR_LIR_ARRAY, .operands = &receiver, .operand_count = 1,
        .source = lower_source(f->owner)};
    *output = (XrLirValue){0};
    return lower_build(f->owner, xr_lir_emit_value(f->owner->module, f->blocks[b], &read, output));
}

static bool lower_array_instruction(LowerFunction *f, uint32_t b, uint32_t i, bool *handled) {
    Lower *c = f->owner;
    const XrXirInstruction *op = &f->source->instructions[i];
    uint32_t result = f->source->parameter_count + i;
    *handled = true;
    if (op->op == XR_XIR_CELL_PLACE && f->array_places && f->array_places[result] != UINT32_MAX)
        return true;
    if (op->op == XR_XIR_CELL_PLACE)
        return lower_value(f, op->args[0], &f->values[result]);
    if (op->op == XR_XIR_PLACE_READ || op->op == XR_XIR_PLACE_WRITE) {
        XrLirValue args[2] = {{0}};
        uint32_t cell = f->array_places ? f->array_places[op->args[0]] : UINT32_MAX;
        if (cell != UINT32_MAX) {
            if (op->op == XR_XIR_PLACE_READ)
                return lower_managed_copy(f, b, f->current[cell], &f->values[result]);
            if (!lower_value(f, op->args[1], &args[1])) return false;
            return lower_managed_copy(f, b, args[1], &f->current[cell]);
        }
        if (!lower_value(f, op->args[0], &args[0])) return false;
        bool read = op->op == XR_XIR_PLACE_READ;
        XrLirType type = lower_value_type(f, args[0]);
        if (type != XR_LIR_STATE_REF && type != XR_LIR_ARRAY_CELL && type != XR_LIR_ARRAY_REF)
            return lower_unsupported(c, "Array place without a typed binding");
        if (read) return lower_array_read(f, b, args[0], &f->values[result]);
        if (!lower_value(f, op->args[1], &args[1])) return false;
        XrLirInstruction write = {.op = type == XR_LIR_STATE_REF ? XR_LIR_STATE_WRITE :
            type == XR_LIR_ARRAY_REF ? XR_LIR_ARRAY_REF_WRITE : XR_LIR_CELL_WRITE,
            .type = XR_LIR_UNIT, .operands = args, .operand_count = 2, .source = lower_source(c)};
        return lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &write));
    }
    XrLirArrayOperation operation;
    switch (op->op) {
        case XR_XIR_ARRAY_NEW: operation = XR_LIR_ARRAY_OP_NEW; break;
        case XR_XIR_ARRAY_REPEAT: operation = XR_LIR_ARRAY_OP_REPEAT; break;
        case XR_XIR_ARRAY_LEN: operation = XR_LIR_ARRAY_OP_LEN; break;
        case XR_XIR_ARRAY_GET: operation = XR_LIR_ARRAY_OP_GET; break;
        case XR_XIR_ARRAY_SET: operation = XR_LIR_ARRAY_OP_SET; break;
        case XR_XIR_ARRAY_PUSH: operation = XR_LIR_ARRAY_OP_PUSH; break;
        case XR_XIR_TO_STRING:
            if (!lower_array_element(c, xr_xir_operand_type(f->source, op->args[0]))) {
                *handled = false;
                return true;
            }
            operation = XR_LIR_ARRAY_OP_DISPLAY;
            break;
        default: *handled = false; return true;
    }
    bool grouped = op->op == XR_XIR_ARRAY_NEW || op->op == XR_XIR_ARRAY_SET;
    uint32_t count = grouped ? op->args[1] :
        operation == XR_LIR_ARRAY_OP_LEN || operation == XR_LIR_ARRAY_OP_DISPLAY ? 1u : 2u;
    XrLirValue *args = lower_alloc(c, count, sizeof(*args));
    if (count && !args) return false;
    bool good = true;
    bool constructor = operation == XR_LIR_ARRAY_OP_NEW || operation == XR_LIR_ARRAY_OP_REPEAT;
    uint32_t receiver = constructor ? UINT32_MAX :
        grouped ? f->source->operands[op->args[0]] : op->args[0];
    uint32_t cell = f->array_places && receiver != UINT32_MAX ? f->array_places[receiver] : UINT32_MAX;
    for (uint32_t a = 0; a < count && good; ++a) {
        if (!a && cell != UINT32_MAX) args[a] = f->current[cell];
        else good = lower_value(f, grouped ? f->source->operands[op->args[0] + a] : op->args[a], &args[a]);
    }
    XrXirType array = op->type;
    if (!constructor)
        array = xr_xir_operand_type(f->source, grouped ? f->source->operands[op->args[0]] : op->args[0]);
    uint32_t intrinsic = 0;
    bool mutation = operation == XR_LIR_ARRAY_OP_SET || operation == XR_LIR_ARRAY_OP_PUSH;
    XrLirValue temporary = {0};
    if (good && !constructor) {
        XrLirType type = lower_value_type(f, args[0]);
        if (!mutation) good = lower_array_read(f, b, args[0], &args[0]);
        else if (type == XR_LIR_STATE_REF) good = lower_array_borrow(f, b, &args[0]);
        else if (cell != UINT32_MAX) {
            good = lower_array_read(f, b, args[0], &temporary);
            XrLirInstruction construct = {.op = XR_LIR_CELL_NEW, .type = XR_LIR_ARRAY_CELL,
                .operands = &temporary, .operand_count = 1, .source = lower_source(c)};
            args[0] = (XrLirValue){0};
            if (good) good = lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &construct, &args[0]));
            if (good && type == XR_LIR_ARRAY) {
                XrLirInstruction drop = {.op = XR_LIR_ARRAY_DROP, .type = XR_LIR_UNIT,
                    .operands = &temporary, .operand_count = 1, .source = lower_source(c)};
                good = lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &drop));
            }
        } else if (type != XR_LIR_ARRAY_CELL && type != XR_LIR_ARRAY_REF)
            good = lower_unsupported(c, "Array mutation without a mutable binding");
    }
    if (good) good = lower_build(c, mutation ? xr_lir_array_mutator(c->module,
        lower_array_element(c, array), lower_value_type(f, args[0]), operation, &intrinsic) :
        xr_lir_array_operation(c->module, lower_array_element(c, array), operation, &intrinsic));
    XrLirInstruction call = {.op = XR_LIR_CALL_INTRINSIC, .type = lower_type(c, op->type),
        .intrinsic = intrinsic, .operands = args, .operand_count = count, .source = lower_source(c)};
    if (good) good = lower_build(c, call.type == XR_LIR_UNIT
        ? xr_lir_emit_effect(c->module, f->blocks[b], &call)
        : xr_lir_emit_value(c->module, f->blocks[b], &call, &f->values[result]));
    if (good && mutation && cell != UINT32_MAX) {
        XrLirValue updated = {0};
        good = lower_array_read(f, b, args[0], &updated);
        f->current[cell] = updated;
        XrLirInstruction drop = {.op = XR_LIR_ARRAY_CELL_DROP, .type = XR_LIR_UNIT,
            .operands = args, .operand_count = 1, .source = lower_source(c)};
        if (good) good = lower_build(c, xr_lir_emit_effect(c->module, f->blocks[b], &drop));
    }
    xr_free(args);
    return good;
}
