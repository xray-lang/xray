/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_string_library.inc.c - Typed String binding locations
 */
static bool rbc_lir_string_library(RbcLirCompiler *c, const XrLirInstructionView *v) {
    if (c->aggregate_operand_count > UINT32_MAX - v->operand_count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "String operands exceed bytecode indices");
    void *storage = c->aggregate_operands;
    if (!rbc_lir_grow(c, &storage, &c->aggregate_operand_capacity,
        c->aggregate_operand_count + v->operand_count, sizeof(*c->aggregate_operands))) return false;
    c->aggregate_operands = storage;
    XrRbcAggregateAccess access = {.operation=XR_RBC_STRING_LIBRARY,
        .first=(uint32_t)c->aggregate_operand_count, .count=v->operand_count,
        .slot=xr_lir_string_role(c->input, v)};
    for (uint32_t n = 0; n < v->operand_count; ++n)
        c->aggregate_operands[c->aggregate_operand_count++] = rbc_lir_array_location(c, v->operands[n]);
    if (v->result) access.result = rbc_lir_array_location(c, v->result);
    return rbc_lir_aggregate_append(c, access);
}
