/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_dispatch.inc.c - Shared switch and computed-goto instruction bodies.
 *
 * KEY CONCEPT:
 *   Unsigned operations implement wrapping arithmetic without C undefined
 *   behavior. Comparisons consume their following jump as one operation.
 */

#define XR_RBC_FETCH()                                                                             \
    do {                                                                                           \
        if (pc >= function->code_count)                                                            \
            return XR_RBC_INVALID;                                                                 \
        instruction = function->code[pc++];                                                        \
        op = instruction & 255;                                                                    \
        a = (instruction >> 8) & 255;                                                              \
        b = (instruction >> 16) & 255;                                                             \
        c = instruction >> 24;                                                                     \
        if (op >= XR_RBC_OP_COUNT)                                                                 \
            return XR_RBC_INVALID;                                                                 \
    } while (0)

#define XR_RBC_REG(index) stack->slots[base + (index)]
#define XR_RBC_CHECK_REG(index)                                                                    \
    do {                                                                                           \
        if ((index) >= function->register_count)                                                   \
            return XR_RBC_INVALID;                                                                 \
    } while (0)
#define XR_RBC_CHECK_AB()                                                                          \
    do {                                                                                           \
        XR_RBC_CHECK_REG(a);                                                                       \
        XR_RBC_CHECK_REG(b);                                                                       \
    } while (0)
#define XR_RBC_BINARY(expression)                                                                  \
    do {                                                                                           \
        XR_RBC_CHECK_AB();                                                                         \
        XR_RBC_CHECK_REG(c);                                                                       \
        XR_RBC_REG(a) = (expression);                                                              \
    } while (0)

#if defined(__clang__) || defined(__GNUC__)
static const void *const dispatch[] = {
#define XR_RBC_LABEL(name, format) &&xr_rbc_##name,
    XR_RBC_OPS(XR_RBC_LABEL)
#undef XR_RBC_LABEL
};
#define XR_RBC_CASE(name) xr_rbc_##name:
#define XR_RBC_NEXT()                                                                              \
    do {                                                                                           \
        XR_RBC_FETCH();                                                                            \
        goto *dispatch[op];                                                                        \
    } while (0)
#else
#define XR_RBC_CASE(name) case XR_RBC_##name:
#define XR_RBC_NEXT() goto xr_rbc_next
#endif

xr_rbc_reload : XR_DCHECK(stack->depth > 0, "interpreter requires an active frame");
function = &module->functions[stack->frames[stack->depth - 1].function];
base = stack->frames[stack->depth - 1].base;
pc = stack->frames[stack->depth - 1].pc;
#if defined(__clang__) || defined(__GNUC__)
XR_RBC_NEXT();
#else
xr_rbc_next : XR_RBC_FETCH();
switch (op) {
#endif
XR_RBC_CASE(MOV) {
    XR_RBC_CHECK_AB();
    XR_RBC_REG(a) = XR_RBC_REG(b);
    XR_RBC_NEXT();
}
XR_RBC_CASE(LOADK) {
    XR_RBC_CHECK_REG(a);
    uint32_t index = instruction >> 16;
    if (index >= function->constant_count)
        return XR_RBC_INVALID;
    XR_RBC_REG(a) = function->constants[index];
    XR_RBC_NEXT();
}
XR_RBC_CASE(LOADI) {
    XR_RBC_CHECK_REG(a);
    uint32_t bits = instruction >> 16;
    int64_t value = bits <= INT16_MAX ? (int64_t) bits : (int64_t) bits - 65536;
    XR_RBC_REG(a) = (uint64_t) value;
    XR_RBC_NEXT();
}
XR_RBC_CASE(ADD) {
    XR_RBC_BINARY(XR_RBC_REG(b) + XR_RBC_REG(c));
    XR_RBC_NEXT();
}
XR_RBC_CASE(SUB) {
    XR_RBC_BINARY(XR_RBC_REG(b) - XR_RBC_REG(c));
    XR_RBC_NEXT();
}
XR_RBC_CASE(MUL) {
    XR_RBC_BINARY(XR_RBC_REG(b) * XR_RBC_REG(c));
    XR_RBC_NEXT();
}
XR_RBC_CASE(NEG) {
    XR_RBC_CHECK_AB();
    XR_RBC_REG(a) = 0 - XR_RBC_REG(b);
    XR_RBC_NEXT();
}
XR_RBC_CASE(DIV)
XR_RBC_CASE(REM) {
    XR_RBC_CHECK_AB();
    XR_RBC_CHECK_REG(c);
    int64_t lhs = xr_rbc_signed(XR_RBC_REG(b)), rhs = xr_rbc_signed(XR_RBC_REG(c));
    if (!rhs)
        return XR_RBC_DIVIDE_BY_ZERO;
    if (lhs == INT64_MIN && rhs == -1)
        XR_RBC_REG(a) = op == XR_RBC_DIV ? (uint64_t) INT64_MIN : 0;
    else
        XR_RBC_REG(a) = (uint64_t) (op == XR_RBC_DIV ? lhs / rhs : lhs % rhs);
    XR_RBC_NEXT();
}
XR_RBC_CASE(AND) {
    XR_RBC_BINARY(XR_RBC_REG(b) & XR_RBC_REG(c));
    XR_RBC_NEXT();
}
XR_RBC_CASE(OR) {
    XR_RBC_BINARY(XR_RBC_REG(b) | XR_RBC_REG(c));
    XR_RBC_NEXT();
}
XR_RBC_CASE(XOR) {
    XR_RBC_BINARY(XR_RBC_REG(b) ^ XR_RBC_REG(c));
    XR_RBC_NEXT();
}
XR_RBC_CASE(SHL) {
    XR_RBC_BINARY(XR_RBC_REG(b) << (XR_RBC_REG(c) & 63));
    XR_RBC_NEXT();
}
XR_RBC_CASE(SHR) {
    XR_RBC_BINARY(xr_rbc_shift_right(XR_RBC_REG(b), XR_RBC_REG(c)));
    XR_RBC_NEXT();
}
XR_RBC_CASE(USHR) {
    XR_RBC_BINARY(XR_RBC_REG(b) >> (XR_RBC_REG(c) & 63));
    XR_RBC_NEXT();
}
XR_RBC_CASE(LT)
XR_RBC_CASE(LE)
XR_RBC_CASE(EQ) {
    XR_RBC_CHECK_AB();
    int64_t lhs = xr_rbc_signed(XR_RBC_REG(a)), rhs = xr_rbc_signed(XR_RBC_REG(b));
    bool take = op == XR_RBC_LT ? lhs < rhs : op == XR_RBC_LE ? lhs <= rhs : lhs == rhs;
    if (pc >= function->code_count || (function->code[pc] & 255) != XR_RBC_JMP)
        return XR_RBC_INVALID;
    instruction = function->code[pc++];
    if (take)
        goto xr_rbc_jump;
    XR_RBC_NEXT();
}
XR_RBC_CASE(JMP)
xr_rbc_jump: {
    uint32_t bits = instruction >> 8;
    int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t) bits : (int32_t) bits - 16777216;
    if (offset < 0) {
        size_t distance = (size_t) -offset;
        if (distance > pc)
            return XR_RBC_INVALID;
        pc -= distance;
    } else {
        if ((size_t) offset >= function->code_count - pc)
            return XR_RBC_INVALID;
        pc += (size_t) offset;
    }
    XR_RBC_NEXT();
}
XR_RBC_CASE(CALL) {
    stack->frames[stack->depth - 1].pc = pc;
    status = xr_rbc_call(module, stack, a, instruction >> 16);
    if (status != XR_RBC_OK)
        return status;
    goto xr_rbc_reload;
}
XR_RBC_CASE(CALLN) {
    XR_RBC_CHECK_REG(a + 1);
    XR_RBC_CHECK_REG(a + 2);
    uint32_t target = instruction >> 16;
    uint64_t id = XR_RBC_REG(a + 2);
    if (target >= module->native_count || id > UINT32_MAX || id > xrt_locations_count(locations))
        return XR_RBC_INVALID;
    module->natives[target](xr_rbc_signed(XR_RBC_REG(a + 1)), (uint32_t) id);
    XR_RBC_NEXT();
}
XR_RBC_CASE(RET)
XR_RBC_CASE(RET0) {
    uint64_t value = 0;
    if (op == XR_RBC_RET) {
        XR_RBC_CHECK_REG(a);
        value = XR_RBC_REG(a);
    }
    uint32_t destination = stack->frames[--stack->depth].destination;
    if (!stack->depth) {
        *result = value;
        return XR_RBC_OK;
    }
    stack->slots[stack->frames[stack->depth - 1].base + destination] = value;
    goto xr_rbc_reload;
}
#if !defined(__clang__) && !defined(__GNUC__)
}
return XR_RBC_INVALID;
#endif

#undef XR_RBC_FETCH
#undef XR_RBC_REG
#undef XR_RBC_CHECK_REG
#undef XR_RBC_CHECK_AB
#undef XR_RBC_BINARY
#undef XR_RBC_CASE
#undef XR_RBC_NEXT
