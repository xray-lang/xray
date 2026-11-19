/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_string.inc.c - Appends through the current String binding
 */
enum LowerStringUpdate {
    LOWER_STRING_KEEP,
    LOWER_STRING_APPEND,
    LOWER_STRING_SKIP
};

static void lower_string_update_block(LowerFunction *f, uint32_t b, const uint8_t *uses,
    uint32_t *last_write) {
    const XrXirFunction *source = f->source;
    const XrXirBlock *block = &source->blocks[b];
    uint32_t parameters = source->parameter_count;
    for (uint32_t i = block->first; i < block->first + block->count; ++i) {
        const XrXirInstruction *op = &source->instructions[i];
        if (op->op != XR_XIR_CELL_WRITE && op->op != XR_XIR_CELL_STRING_APPEND) continue;
        uint32_t binding = op->args[0], cell = f->cell_for[binding];
        if (!f->promoted_arrays[binding] || cell == UINT32_MAX || f->cell_types[cell] != XR_LIR_STRING) continue;
        uint32_t previous = last_write[cell];
        last_write[cell] = i + 1;
        if (op->op != XR_XIR_CELL_WRITE || i == block->first || op->args[1] != parameters + i - 1) continue;
        const XrXirInstruction *concat = &source->instructions[i - 1];
        if (concat->op != XR_XIR_CONCAT_STRING || uses[parameters + i - 1] != 1 ||
            concat->args[0] < parameters + block->first || concat->args[0] >= parameters + i - 1) continue;
        uint32_t read = concat->args[0] - parameters;
        const XrXirInstruction *left = &source->instructions[read];
        if (left->op != XR_XIR_CELL_READ || left->args[0] != binding ||
            uses[concat->args[0]] != 1 || previous > read) continue;
        /* Promotion proves no other instruction can rebind this String.
         * Reuse its owner at the original concat, after the suffix ran. */
        f->string_updates[read] = LOWER_STRING_SKIP;
        f->string_updates[i - 1] = LOWER_STRING_APPEND;
        f->string_updates[i] = LOWER_STRING_SKIP;
    }
    for (uint32_t i = block->first; i < block->first + block->count; ++i) {
        const XrXirInstruction *op = &source->instructions[i];
        if (op->op != XR_XIR_CELL_WRITE && op->op != XR_XIR_CELL_STRING_APPEND) continue;
        uint32_t cell = f->cell_for[op->args[0]];
        if (cell != UINT32_MAX) last_write[cell] = 0;
    }
}

static bool lower_string_updates(LowerFunction *f) {
    if (!f->promoted_arrays || !f->cell_count || f->lexical.count) return true;
    const XrXirFunction *source = f->source;
    bool concat = false;
    for (uint32_t i = 0; i < source->instruction_count && !concat; ++i)
        concat = source->instructions[i].op == XR_XIR_CONCAT_STRING;
    if (!concat) return true;
    f->string_updates = lower_alloc(f->owner, source->instruction_count, 1);
    uint8_t *uses = lower_alloc(f->owner, f->value_count, 1);
    uint32_t *last_write = lower_alloc(f->owner, f->cell_count, sizeof(*last_write));
    bool ready = f->string_updates && uses && last_write;
    if (ready) {
        for (uint32_t b = 0; b < source->block_count; ++b) if (f->reachable[b]) {
            const XrXirBlock *block = &source->blocks[b];
            for (uint32_t i = block->first; i < block->first + block->count; ++i) {
                const XrXirInstruction *op = &source->instructions[i];
                uint32_t count = lower_operand_count(source, op);
                uint32_t stride = op->op == XR_XIR_PHI ? 2u : 1u;
                for (uint32_t a = stride - 1; a < count; a += stride) {
                    uint32_t value = lower_operand_at(source, op, a);
                    if (uses[value] < 2) ++uses[value];
                }
            }
        }
        for (uint32_t b = 0; b < source->block_count; ++b)
            if (f->reachable[b]) lower_string_update_block(f, b, uses, last_write);
    }
    xr_free(last_write);
    xr_free(uses);
    return ready;
}

static bool lower_string_append(LowerFunction *f, uint32_t b, uint32_t i, bool *handled) {
    const XrXirInstruction *op = &f->source->instructions[i];
    enum LowerStringUpdate update = f->string_updates ? f->string_updates[i] : LOWER_STRING_KEEP;
    *handled = update != LOWER_STRING_KEEP || op->op == XR_XIR_CELL_STRING_APPEND;
    if (!*handled || update == LOWER_STRING_SKIP) return true;
    Lower *c = f->owner;
    XrLirValue arguments[2];
    if (!lower_value(f, op->args[1], &arguments[1])) return false;
    if (update == LOWER_STRING_KEEP && op->immediate) {
        XrLirType type = lower_type(c, xr_xir_operand_type(f->source, op->args[1]));
        if (type == XR_LIR_I64 || type == XR_LIR_F64) {
            XrLirValue input = arguments[1];
            XrLirInstruction format = {.op=type == XR_LIR_I64 ? XR_LIR_STRING_I64 : XR_LIR_STRING_F64,
                .type=XR_LIR_STRING, .operands=&input, .operand_count=1, .source=lower_source(c)};
            if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &format, &arguments[1]))) return false;
        } else if (!lower_display(f, b, arguments[1], &arguments[1])) return false;
    }
    uint32_t binding = update == LOWER_STRING_APPEND ? f->source->instructions[i + 1].args[0] : op->args[0];
    uint32_t cell = f->cell_for[binding];
    bool promoted = cell != UINT32_MAX;
    if (promoted) arguments[0] = f->current[cell];
    else if (!lower_value(f, binding, &arguments[0])) return false;
    XrLirType receiver = c->module->functions[f->function.id - 1].values[arguments[0].id - 1].type;
    XrLirStringOperation role = promoted ? XR_LIR_STRING_OP_APPEND_OWNED : receiver == XR_LIR_STATE_REF ?
        XR_LIR_STRING_OP_APPEND_STATE : receiver == XR_LIR_STRING_REF ?
        XR_LIR_STRING_OP_APPEND_REF : XR_LIR_STRING_OP_APPEND_CELL;
    uint32_t intrinsic;
    if (!lower_build(c, xr_lir_string_operation(c->module, role, &intrinsic))) return false;
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC, .type=promoted ? XR_LIR_STRING : XR_LIR_UNIT,
        .intrinsic=intrinsic, .operands=arguments, .operand_count=2, .source=lower_source(c)};
    return lower_build(c, promoted ?
        xr_lir_emit_value(c->module, f->blocks[b], &call, &f->current[cell]) :
        xr_lir_emit_effect(c->module, f->blocks[b], &call));
}

static bool lower_string_borrow(LowerFunction *f, uint32_t block, XrLirValue *argument) {
    Lower *c = f->owner;
    XrLirType type = c->module->functions[f->function.id - 1].values[argument->id - 1].type;
    if (type == XR_LIR_STRING_REF) return true;
    XrLirStringOperation role = type == XR_LIR_STATE_REF ?
        XR_LIR_STRING_OP_BORROW_STATE : XR_LIR_STRING_OP_BORROW_CELL;
    uint32_t intrinsic;
    if (!lower_build(c, xr_lir_string_operation(c->module, role, &intrinsic))) return false;
    XrLirValue input = *argument;
    XrLirInstruction call = {.op=XR_LIR_CALL_INTRINSIC, .type=XR_LIR_STRING_REF,
        .intrinsic=intrinsic, .operands=&input, .operand_count=1, .source=lower_source(c)};
    return lower_build(c, xr_lir_emit_value(c->module, f->blocks[block], &call, argument));
}
