/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_scalar_dispatch.inc.c - Scalar handlers shared with sealed loop replay
 *
 * KEY CONCEPT:
 *   Ordinary dispatch and replay use the same register and immediate semantics.
 *   Replay has a bounded cursor and admits only local, forward branches.
 */

#if defined(XR_RBC_SCALAR_TRANSFER)
XR_RBC_CASE(LOADK) {
    uint32_t index = instruction >> 16;
    XR_RBC_REG(XR_RBC_A()) = constants[index];
    XR_RBC_NEXT();
}
XR_RBC_CASE(LOADI) {
    uint32_t bits = instruction >> 16;
    int64_t value = bits <= INT16_MAX ? (int64_t) bits : (int64_t) bits - 65536;
    XR_RBC_REG(XR_RBC_A()) = (uint64_t) value;
    XR_RBC_NEXT();
}
XR_RBC_CASE(SPILL) {
    size_t index = frame->spill_base + (instruction >> 16);
    stack->spills[index] = XR_RBC_SLOT(XR_RBC_A());
    XR_RBC_SLOT(XR_RBC_A()) = (XrRbcRegister){0};
    XR_RBC_NEXT();
}
XR_RBC_CASE(RELOAD) {
    size_t index = frame->spill_base + (instruction >> 16);
    XR_RBC_SLOT(XR_RBC_A()) = stack->spills[index];
    // Scalar reloads copy bits. Reference reloads transfer the sole owner.
    if ((instruction >> 16) < function->reference_spill_count)
        stack->spills[index] = (XrRbcRegister){0};
    XR_RBC_NEXT();
}
#elif defined(XR_RBC_SCALAR_ARITHMETIC)
XR_RBC_CASE(ADD) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) + XR_RBC_REG(XR_RBC_C()));
    XR_RBC_NEXT();
}
XR_RBC_CASE(ADDI) {
    uint64_t value = XR_RBC_C() <= INT8_MAX ? (uint64_t)XR_RBC_C() : (uint64_t)XR_RBC_C() - 256;
    XR_RBC_REG(XR_RBC_A()) = XR_RBC_REG(XR_RBC_B()) + value;
    XR_RBC_NEXT();
}
XR_RBC_CASE(SUBI) {
    uint64_t value = XR_RBC_C() <= INT8_MAX ? (uint64_t)XR_RBC_C() : (uint64_t)XR_RBC_C() - 256;
    XR_RBC_REG(XR_RBC_A()) = XR_RBC_REG(XR_RBC_B()) - value;
    XR_RBC_NEXT();
}
XR_RBC_CASE(SUB) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) - XR_RBC_REG(XR_RBC_C()));
    XR_RBC_NEXT();
}
XR_RBC_CASE(MUL) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) * XR_RBC_REG(XR_RBC_C()));
    XR_RBC_NEXT();
}
XR_RBC_CASE(MULI) {
    uint64_t value = XR_RBC_C() <= INT8_MAX ? (uint64_t)XR_RBC_C() : (uint64_t)XR_RBC_C() - 256;
    XR_RBC_REG(XR_RBC_A()) = XR_RBC_REG(XR_RBC_B()) * value;
    XR_RBC_NEXT();
}
XR_RBC_CASE(NEG) {
    XR_RBC_REG(XR_RBC_A()) = 0 - XR_RBC_REG(XR_RBC_B());
    XR_RBC_NEXT();
}
#elif defined(XR_RBC_SCALAR_BRANCH)
XR_RBC_CASE(LTI)
XR_RBC_CASE(LEI)
XR_RBC_CASE(GTI)
XR_RBC_CASE(GEI)
XR_RBC_CASE(EQI) {
    uint32_t bits = instruction >> 16;
    int64_t rhs = bits <= INT16_MAX ? (int64_t)bits : (int64_t)bits - 65536;
    int64_t lhs = xr_rbc_signed(XR_RBC_REG(XR_RBC_A()));
    bool take = op == XR_RBC_LTI ? lhs < rhs : op == XR_RBC_LEI ? lhs <= rhs :
        op == XR_RBC_GTI ? lhs > rhs : op == XR_RBC_GEI ? lhs >= rhs : lhs == rhs;
    instruction = code[pc++];
    if (take) goto xr_rbc_jump;
    XR_RBC_NEXT();
}
XR_RBC_CASE(LT)
XR_RBC_CASE(LE)
XR_RBC_CASE(EQ) {
    int64_t lhs = xr_rbc_signed(XR_RBC_REG(XR_RBC_A())), rhs = xr_rbc_signed(XR_RBC_REG(XR_RBC_B()));
    bool take = op == XR_RBC_LT ? lhs < rhs : op == XR_RBC_LE ? lhs <= rhs : lhs == rhs;
    instruction = code[pc++];
    if (take)
        goto xr_rbc_jump;
    XR_RBC_NEXT();
}
XR_RBC_CASE(JMP)
xr_rbc_jump: {
    uint32_t bits = instruction >> 8;
    int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t) bits : (int32_t) bits - 16777216;
    // Preparation checked the target; unsigned addition also handles backedges.
    pc += (size_t) offset;
    XR_RBC_NEXT();
}
#else
#error "Select a scalar handler group before inclusion"
#endif
