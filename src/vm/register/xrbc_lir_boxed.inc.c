/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_boxed.inc.c - Typed canonical values through cold bytecode accesses
 */
static XrLirType rbc_lir_boxed_payload(const XrLirModule *module, XrLirFunction function,
    const XrLirInstructionView *instruction) {
    XrLirType type = instruction->type;
    if (instruction->op == XR_LIR_STATE_WRITE || instruction->op == XR_LIR_STATE_CONSTRUCT ||
        instruction->op == XR_LIR_STATE_INITIALIZE) {
        if (instruction->operand_count != 2) return XR_LIR_TYPE_INVALID;
        xr_lir_value_type(module, (XrLirValue){module, function.id, instruction->operands[1]}, &type);
    } else if (instruction->op == XR_LIR_VALUE_DROP || instruction->op == XR_LIR_RECORD_FIELD ||
        instruction->op == XR_LIR_NULLABLE_IS_SOME || instruction->op == XR_LIR_NULLABLE_UNWRAP) {
        xr_lir_value_type(module, (XrLirValue){module, function.id, instruction->operands[0]}, &type);
    } else if (instruction->op != XR_LIR_STATE_READ && instruction->op != XR_LIR_VALUE_RETAIN &&
        instruction->op != XR_LIR_RECORD_NEW && instruction->op != XR_LIR_NULLABLE_NONE &&
        instruction->op != XR_LIR_NULLABLE_SOME) return XR_LIR_TYPE_INVALID;
    return xr_lir_boxed_type(module, type) ? type : XR_LIR_TYPE_INVALID;
}
static uint32_t rbc_lir_boxed_identity(XrLirType type) {
    return XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT + type - XR_LIR_TYPE_COUNT;
}
static bool rbc_lir_boxed_emit(RbcLirCompiler *c, const XrLirInstructionView *instruction) {
    XrLirType type = rbc_lir_boxed_payload(c->input, c->function, instruction);
    XrRbcAggregateAccess access = {.slot=rbc_lir_boxed_identity(type), .count=instruction->operand_count,
        .destination=(uint32_t)instruction->bits};
    if (instruction->result) access.result = rbc_lir_array_location(c, instruction->result);
    switch (instruction->op) {
    case XR_LIR_RECORD_NEW: case XR_LIR_NULLABLE_NONE: case XR_LIR_NULLABLE_SOME:
        access.operation = XR_RBC_BOXED_NEW; break;
    case XR_LIR_VALUE_RETAIN: access.operation = XR_RBC_BOXED_RETAIN; break;
    case XR_LIR_VALUE_DROP: access.operation = XR_RBC_BOXED_DROP; break;
    case XR_LIR_RECORD_FIELD: case XR_LIR_NULLABLE_UNWRAP: access.operation = XR_RBC_BOXED_FIELD; break;
    case XR_LIR_NULLABLE_IS_SOME: access.operation = XR_RBC_BOXED_PRESENT; break;
    case XR_LIR_STATE_READ: access.operation = XR_RBC_BOXED_STATE_READ; break;
    case XR_LIR_STATE_WRITE: access.operation = XR_RBC_BOXED_STATE_WRITE; break;
    case XR_LIR_STATE_CONSTRUCT: access.operation = XR_RBC_BOXED_STATE_CONSTRUCT; break;
    case XR_LIR_STATE_INITIALIZE: access.operation = XR_RBC_BOXED_STATE_INITIALIZE; break;
    default: return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "boxed value has no bytecode role");
    }
    if (c->aggregate_operand_count > UINT32_MAX - access.count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "boxed operands exceed indexed storage");
    void *storage = c->aggregate_operands;
    if (!rbc_lir_grow(c, &storage, &c->aggregate_operand_capacity,
        c->aggregate_operand_count + access.count, sizeof(*c->aggregate_operands))) return false;
    c->aggregate_operands = storage;
    access.first = (uint32_t)c->aggregate_operand_count;
    for (uint32_t p = 0; p < access.count; ++p)
        c->aggregate_operands[c->aggregate_operand_count++] = rbc_lir_array_location(c, instruction->operands[p]);
    return rbc_lir_aggregate_append(c, access);
}
static bool rbc_lir_boxed_take(RbcLirCompiler *c, uint32_t destination, uint32_t source) {
    return rbc_lir_aggregate_append(c, (XrRbcAggregateAccess){.operation=XR_RBC_BOXED_TAKE,
        .slot=c->register_boxed_types[destination], .source=source, .result={destination, false}});
}
