/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_set.inc.c - Fixed Set values and actual binding operations
 */
static bool rbc_lir_set_operation(XrLirOp operation) {
    return operation >= XR_LIR_SET_NEW && operation <= XR_LIR_SET_CLEAR;
}
static bool rbc_lir_set_point(RbcLirCompiler *c,const XrLirInstructionView *i) {
    if (rbc_lir_set_operation(i->op) || i->op==XR_LIR_SET_RETAIN || i->op==XR_LIR_SET_DROP ||
        i->op==XR_LIR_SET_CELL_DROP || i->op==XR_LIR_SET_BORROW ||
        i->op==XR_LIR_SET_REF_READ || i->op==XR_LIR_SET_REF_WRITE) return true;
    if (i->op==XR_LIR_STATE_READ || i->op==XR_LIR_CELL_READ) return i->type==XR_LIR_SET_I64;
    if (i->op==XR_LIR_CELL_NEW) return i->type==XR_LIR_SET_I64_CELL;
    if (i->operand_count==2 && (i->op==XR_LIR_STATE_WRITE || i->op==XR_LIR_CELL_WRITE)) {
        XrLirType type=XR_LIR_TYPE_INVALID;
        uint32_t value=i->operands[i->op==XR_LIR_STATE_WRITE ? 1u : 0u];
        XrLirType expected=i->op==XR_LIR_STATE_WRITE ? XR_LIR_SET_I64 : XR_LIR_SET_I64_CELL;
        return xr_lir_value_type(c->input,(XrLirValue){c->input,c->function.id,value},&type)==XR_LIR_OK &&
            type==expected;
    }
    return false;
}
