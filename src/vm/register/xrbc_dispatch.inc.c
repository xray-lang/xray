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

#define XR_RBC_FETCH() \
    do { \
        instruction = function->code[pc++]; \
        op = instruction & 255; \
        a = (instruction >> 8) & 255; \
        b = (instruction >> 16) & 255; \
        c = instruction >> 24; \
    } while (0)

#define XR_RBC_SLOT(index) stack->slots[base + (index)]
#define XR_RBC_REG(index) XR_RBC_SLOT(index).bits
#define XR_RBC_STRING(index) XR_RBC_SLOT(index).string
#define XR_RBC_SOURCE() (function->source_ids ? function->source_ids[pc - 1] : 0)
#define XR_RBC_BINARY(expression) (XR_RBC_REG(a) = (expression))

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
    XR_RBC_SLOT(a) = XR_RBC_SLOT(b);
    XR_RBC_NEXT();
}
XR_RBC_CASE(LOADK) {
    uint32_t index = instruction >> 16;
    XR_RBC_REG(a) = function->constants[index];
    XR_RBC_NEXT();
}
XR_RBC_CASE(LOADI) {
    uint32_t bits = instruction >> 16;
    int64_t value = bits <= INT16_MAX ? (int64_t) bits : (int64_t) bits - 65536;
    XR_RBC_REG(a) = (uint64_t) value;
    XR_RBC_NEXT();
}
XR_RBC_CASE(SPILL) {
    size_t index = stack->frames[stack->depth - 1].spill_base + (instruction >> 16);
    stack->spills[index] = XR_RBC_SLOT(a);
    XR_RBC_SLOT(a) = (XrRbcRegister){0};
    XR_RBC_NEXT();
}
XR_RBC_CASE(RELOAD) {
    size_t index = stack->frames[stack->depth - 1].spill_base + (instruction >> 16);
    XR_RBC_SLOT(a) = stack->spills[index];
    stack->spills[index] = (XrRbcRegister){0};
    XR_RBC_NEXT();
}
XR_RBC_CASE(CELL_NEW) {
    uint32_t ordinal = instruction >> 16;
    XrRbcFrame *frame = &stack->frames[stack->depth - 1];
    if (ordinal > frame->initialized_cells) return XR_RBC_INVALID;
    size_t index = frame->cell_base + ordinal;
    stack->cells[index].bits = XR_RBC_REG(a);
    if (ordinal == frame->initialized_cells) ++frame->initialized_cells;
    XR_RBC_REG(a) = (uint64_t)index + 1;
    XR_RBC_NEXT();
}
XR_RBC_CASE(CELL_READ) {
    bool string = function->register_kinds && function->register_kinds[b] == XR_RBC_STRING_CELL;
    XrRbcRegister *cell = xr_rbc_cell(stack, module, XR_RBC_REG(b), string ? XR_RBC_STRING : XR_RBC_BITS);
    if (!cell) return XR_RBC_INVALID;
    if (string) XR_RBC_STRING(a) = xrt_string_retain(cell->string, XR_RBC_SOURCE());
    else XR_RBC_REG(a) = cell->bits;
    XR_RBC_NEXT();
}
XR_RBC_CASE(CELL_WRITE) {
    bool string = function->register_kinds && function->register_kinds[a] == XR_RBC_STRING_CELL;
    XrRbcRegister *cell = xr_rbc_cell(stack, module, XR_RBC_REG(a), string ? XR_RBC_STRING : XR_RBC_BITS);
    if (!cell) return XR_RBC_INVALID;
    if (string) {
        XrtString *owned = xrt_string_retain(XR_RBC_STRING(b), XR_RBC_SOURCE());
        xrt_string_drop(&cell->string);
        cell->string = owned;
    } else cell->bits = XR_RBC_REG(b);
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_LITERAL) {
    const XrRbcLiteral *literal = &module->literals[instruction >> 16];
    XR_RBC_STRING(a) = xrt_string_new(literal->bytes, literal->length, XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_I64) {
    XR_RBC_STRING(a) = xrt_string_i64(xr_rbc_signed(XR_RBC_REG(b)), XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_RETAIN) {
    XR_RBC_STRING(a) = xrt_string_retain(XR_RBC_STRING(b), XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_CONCAT) {
    XR_RBC_STRING(a) = xrt_string_concat(XR_RBC_STRING(b), XR_RBC_STRING(c), XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_LEN) {
    XR_RBC_REG(a) = (uint64_t)xrt_string_length(XR_RBC_STRING(b), XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_DROP) {
    xrt_string_drop(&XR_RBC_STRING(a));
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_PRINT) {
    size_t length;
    const char *bytes = xrt_string_bytes(XR_RBC_STRING(a), &length);
    xrt_print_string(bytes, length, XR_RBC_SOURCE());
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_TAKE) {
    XR_RBC_STRING(a) = XR_RBC_STRING(b);
    XR_RBC_STRING(b) = NULL;
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_CELL_NEW) {
    XrRbcFrame *frame = &stack->frames[stack->depth - 1];
    if (c != frame->initialized_cells) return XR_RBC_INVALID;
    size_t index = frame->cell_base + c;
    stack->cells[index].string = xrt_string_retain(XR_RBC_STRING(b), XR_RBC_SOURCE());
    ++frame->initialized_cells;
    XR_RBC_REG(a) = (uint64_t)index + 1;
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_CELL_DROP) {
    XrRbcRegister *cell = xr_rbc_cell(stack, module, XR_RBC_REG(a), XR_RBC_STRING);
    if (!cell) return XR_RBC_INVALID;
    xrt_string_drop(&cell->string);
    XR_RBC_REG(a) = 0;
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
    XR_RBC_REG(a) = 0 - XR_RBC_REG(b);
    XR_RBC_NEXT();
}
XR_RBC_CASE(FNEG) {
    uint64_t result;
    if (xr_xir_float_negate(64, XR_RBC_REG(b), &result) != XR_XIR_NUMERIC_OK)
        return XR_RBC_INVALID;
    XR_RBC_REG(a) = result;
    XR_RBC_NEXT();
}
XR_RBC_CASE(DIV)
XR_RBC_CASE(REM) {
    int64_t lhs = xr_rbc_signed(XR_RBC_REG(b)), rhs = xr_rbc_signed(XR_RBC_REG(c));
    if (!rhs) {
        uint32_t source_id = function->source_ids ? function->source_ids[pc - 1] : 0;
        xrt_arithmetic_panic(op == XR_RBC_DIV ? 420u : 421u, source_id);
    }
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
    int64_t lhs = xr_rbc_signed(XR_RBC_REG(a)), rhs = xr_rbc_signed(XR_RBC_REG(b));
    bool take = op == XR_RBC_LT ? lhs < rhs : op == XR_RBC_LE ? lhs <= rhs : lhs == rhs;
    instruction = function->code[pc++];
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
XR_RBC_CASE(CALL) {
    stack->frames[stack->depth - 1].pc = pc;
    status = xr_rbc_call(module, stack, a, instruction >> 16);
    if (status != XR_RBC_OK)
        return status;
    goto xr_rbc_reload;
}
XR_RBC_CASE(CALLN) {
    uint32_t target = instruction >> 16;
    XrRbcNativeKind kind = module->natives[target];
    uint64_t id = XR_RBC_REG(a + (kind == XR_RBC_NATIVE_PRINT_I64 ||
        kind == XR_RBC_NATIVE_UTC_OFFSET_AT ? 2 : 1));
    if (id > UINT32_MAX || id > xrt_locations_count(locations))
        return XR_RBC_INVALID;
    switch (kind) {
    case XR_RBC_NATIVE_PRINT_I64:
        xrt_print_i64(xr_rbc_signed(XR_RBC_REG(a + 1)), (uint32_t)id); break;
    case XR_RBC_NATIVE_CLOCK_REALTIME:
        XR_RBC_REG(a) = (uint64_t)xrt_clock_realtime_nanos((uint32_t)id); break;
    case XR_RBC_NATIVE_CLOCK_CPU:
        XR_RBC_REG(a) = (uint64_t)xrt_clock_process_cpu_nanos((uint32_t)id); break;
    case XR_RBC_NATIVE_CLOCK_MONOTONIC:
        XR_RBC_REG(a) = (uint64_t)xrt_clock_monotonic_nanos((uint32_t)id); break;
    case XR_RBC_NATIVE_UTC_OFFSET_AT:
        XR_RBC_REG(a) = (uint64_t)xrt_clock_utc_offset_at(xr_rbc_signed(XR_RBC_REG(a + 1)), (uint32_t)id); break;
    default: return XR_RBC_INVALID;
    }
    XR_RBC_NEXT();
}
XR_RBC_CASE(PRINT_LITERAL) {
    const XrRbcLiteral *literal = &module->literals[instruction >> 16];
    uint32_t source_id = function->source_ids ? function->source_ids[pc - 1] : 0;
    xrt_print_string(literal->bytes, literal->length, source_id);
    XR_RBC_NEXT();
}
XR_RBC_CASE(RET)
XR_RBC_CASE(RET0) {
    XrRbcRegister value = {0};
    if (op == XR_RBC_RET) {
        value = XR_RBC_SLOT(a);
        if (function->result_kind == XR_RBC_STRING) XR_RBC_STRING(a) = NULL;
    }
    XrRbcFrame retired = stack->frames[--stack->depth];
    uint32_t destination = retired.destination;
    stack->cell_count = retired.cell_base;
    stack->spill_count = retired.spill_base;
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
#undef XR_RBC_SLOT
#undef XR_RBC_STRING
#undef XR_RBC_SOURCE
#undef XR_RBC_BINARY
#undef XR_RBC_CASE
#undef XR_RBC_NEXT
