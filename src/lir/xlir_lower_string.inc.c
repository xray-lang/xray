/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_string.inc.c - Appends through the current String binding
 */
static bool lower_string_append(LowerFunction *f, uint32_t b, uint32_t i, bool *handled) {
    const XrXirInstruction *op = &f->source->instructions[i];
    *handled = op->op == XR_XIR_CELL_STRING_APPEND;
    if (!*handled) return true;
    Lower *c = f->owner;
    XrLirValue arguments[2];
    if (!lower_value(f, op->args[1], &arguments[1])) return false;
    if (op->immediate) {
        XrLirType type = lower_type(c, xr_xir_operand_type(f->source, op->args[1]));
        if (type == XR_LIR_I64 || type == XR_LIR_F64) {
            XrLirValue input = arguments[1];
            XrLirInstruction format = {.op=type == XR_LIR_I64 ? XR_LIR_STRING_I64 : XR_LIR_STRING_F64,
                .type=XR_LIR_STRING, .operands=&input, .operand_count=1, .source=lower_source(c)};
            if (!lower_build(c, xr_lir_emit_value(c->module, f->blocks[b], &format, &arguments[1]))) return false;
        } else if (!lower_display(f, b, arguments[1], &arguments[1])) return false;
    }
    uint32_t cell = f->cell_for[op->args[0]];
    bool promoted = cell != UINT32_MAX;
    if (promoted) arguments[0] = f->current[cell];
    else if (!lower_value(f, op->args[0], &arguments[0])) return false;
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
