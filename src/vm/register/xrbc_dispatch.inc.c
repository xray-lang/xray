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
        instruction = code[pc++]; \
        op = instruction & 255; \
    } while (0)

/* Decode only operands used by the selected handler. Keeping all three live
 * through every dispatch needlessly consumes registers and instruction bytes. */
#define XR_RBC_A() ((instruction >> 8) & 255u)
#define XR_RBC_B() ((instruction >> 16) & 255u)
#define XR_RBC_C() (instruction >> 24)
#define XR_RBC_SLOT(index) registers[index]
#define XR_RBC_REG(index) XR_RBC_SLOT(index).bits
#define XR_RBC_STRING(index) XR_RBC_SLOT(index).string
#define XR_RBC_SOURCE() (function->source_ids ? function->source_ids[pc - 1] : 0)
#define XR_RBC_BINARY(expression) (XR_RBC_REG(XR_RBC_A()) = (expression))
#define XR_RBC_AGGREGATE_EXECUTE() do { \
    uint32_t aggregate_index = instruction >> 16; \
    bool aggregate_control = function->aggregate_accesses[aggregate_index].operation >= XR_RBC_AGGREGATE_CALL; \
    XrRbcStatus aggregate_status = aggregate_control ? \
        xr_rbc_aggregate_control(module, stack, function, aggregate_index, XR_RBC_SOURCE(), pc) : \
        xr_rbc_aggregate_step(module, stack, frame, function, aggregate_index, XR_RBC_SOURCE()); \
    if (aggregate_status != XR_RBC_OK) return aggregate_status; \
    if (aggregate_control) { \
        if (!stack->depth) { *result = stack->slots[0]; return XR_RBC_OK; } \
        frame = &stack->frames[stack->depth - 1]; \
        function = &module->functions[frame->function]; \
        registers = stack->slots + frame->base; \
        code = function->code; \
        constants = function->constants; \
        pc = frame->pc; \
    } \
} while (0)

/* Value words that may enter or leave a frame reload the current frame. */
#define XR_RBC_VALUE_EXECUTE() do { \
    XrRbcStatus status = xr_rbc_values_step(module, stack, instruction >> 16, pc, XR_RBC_SOURCE()); \
    if (status != XR_RBC_OK) return status; \
    XR_RBC_FRAME_RELOAD(); \
} while (0)
#define XR_RBC_FRAME_STEP(step) do { \
    XrRbcStatus status = step(module, stack, instruction >> 16, pc); \
    if (status != XR_RBC_OK) return status; \
    XR_RBC_FRAME_RELOAD(); \
} while (0)
#define XR_RBC_FRAME_RELOAD() do { \
    if (!stack->depth) { \
        if (function->result_kind == XR_RBC_BITS) *result = stack->slots[0]; \
        return XR_RBC_OK; \
    } \
    frame = &stack->frames[stack->depth - 1]; \
    function = &module->functions[frame->function]; \
    registers = stack->slots + frame->base; \
    code = function->code; \
    constants = function->constants; \
    pc = frame->pc; \
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

XR_DCHECK(stack->depth > 0, "interpreter requires an active frame");
frame = &stack->frames[stack->depth - 1];
function = &module->functions[frame->function];
code = function->code;
constants = function->constants;
registers = stack->slots + frame->base;
pc = frame->pc;
#if defined(__clang__) || defined(__GNUC__)
XR_RBC_NEXT();
#else
xr_rbc_next : XR_RBC_FETCH();
switch (op) {
#endif
XR_RBC_CASE(MOV) {
    XR_RBC_SLOT(XR_RBC_A()) = XR_RBC_SLOT(XR_RBC_B());
    XR_RBC_NEXT();
}
#if defined(__clang__) || defined(__GNUC__)
XR_RBC_CASE(AGGREGATE_ACCESS) {
    XR_RBC_AGGREGATE_EXECUTE();
    XR_RBC_NEXT();
}
#endif
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
XR_RBC_CASE(CELL_NEW) {
    uint32_t ordinal = instruction >> 16;
    if (ordinal>=function->cell_count) return XR_RBC_INVALID;
    size_t index = frame->cell_base + ordinal;
    if (stack->cell_ready[index]) return XR_RBC_INVALID;
    stack->cells[index].bits = XR_RBC_REG(XR_RBC_A());
    stack->cell_ready[index]=1;
    ++frame->initialized_cells;
    XR_RBC_REG(XR_RBC_A()) = (uint64_t)index + 1;
    XR_RBC_NEXT();
}
XR_RBC_CASE(CELL_READ) {
    XrRbcRegister *cell = xr_rbc_cell(stack,module,XR_RBC_REG(XR_RBC_B()),XR_RBC_BITS);
    if (!cell) return XR_RBC_INVALID;
    XR_RBC_REG(XR_RBC_A()) = cell->bits;
    XR_RBC_NEXT();
}
XR_RBC_CASE(CELL_WRITE) {
    XrRbcRegister *cell = xr_rbc_cell(stack,module,XR_RBC_REG(XR_RBC_A()),XR_RBC_BITS);
    if (!cell) return XR_RBC_INVALID;
    cell->bits = XR_RBC_REG(XR_RBC_B());
    XR_RBC_NEXT();
}
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
XR_RBC_CASE(FNEG) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float_bits(-xr_rbc_float(XR_RBC_REG(XR_RBC_B())));
    XR_RBC_NEXT();
}
XR_RBC_CASE(FADD) {
    double left = xr_rbc_float(XR_RBC_REG(XR_RBC_B())), right = xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float_bits(left + right);
    XR_RBC_NEXT();
}
XR_RBC_CASE(FSUB) {
    double left = xr_rbc_float(XR_RBC_REG(XR_RBC_B())), right = xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float_bits(left - right);
    XR_RBC_NEXT();
}
XR_RBC_CASE(FMUL) {
    double left = xr_rbc_float(XR_RBC_REG(XR_RBC_B())), right = xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float_bits(left * right);
    XR_RBC_NEXT();
}
XR_RBC_CASE(FDIV) {
    double left = xr_rbc_float(XR_RBC_REG(XR_RBC_B())), right = xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float_bits(left / right);
    XR_RBC_NEXT();
}
XR_RBC_CASE(FEQ) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float(XR_RBC_REG(XR_RBC_B())) == xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    goto xr_rbc_float_compare;
}
XR_RBC_CASE(FNE) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float(XR_RBC_REG(XR_RBC_B())) != xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    goto xr_rbc_float_compare;
}
XR_RBC_CASE(FLT) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float(XR_RBC_REG(XR_RBC_B())) < xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    goto xr_rbc_float_compare;
}
XR_RBC_CASE(FLE) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float(XR_RBC_REG(XR_RBC_B())) <= xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    goto xr_rbc_float_compare;
}
XR_RBC_CASE(FLEK) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float(XR_RBC_REG(XR_RBC_B())) <= xr_rbc_float(constants[XR_RBC_C()]);
    goto xr_rbc_float_compare;
}
#if defined(__clang__) || defined(__GNUC__)
#include "xrbc_state_dispatch.inc.c"
#endif
XR_RBC_CASE(FGT) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float(XR_RBC_REG(XR_RBC_B())) > xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    goto xr_rbc_float_compare;
}
XR_RBC_CASE(FGE) {
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_float(XR_RBC_REG(XR_RBC_B())) >= xr_rbc_float(XR_RBC_REG(XR_RBC_C()));
    goto xr_rbc_float_compare;
}
xr_rbc_float_compare: {
    // Admission guarantees the successor exists, and EQI retains its complete
    // jump/skip pair. Keep the boolean register for any later independent use.
    if (code[pc] == ((uint32_t)XR_RBC_EQI | (XR_RBC_A() << 8))) {
        bool take = XR_RBC_REG(XR_RBC_A()) == 0;
        ++pc;
        instruction = code[pc++];
        if (take) goto xr_rbc_jump;
    }
    XR_RBC_NEXT();
}
XR_RBC_CASE(ITOF) {
    int64_t source = xr_rbc_signed(XR_RBC_REG(XR_RBC_B()));
    XR_RBC_REG(XR_RBC_A()) = xrt_i64_to_f64(source);
    XR_RBC_NEXT();
}
XR_RBC_CASE(FTOI) {
    uint64_t source = XR_RBC_REG(XR_RBC_B());
    int64_t result = xrt_f64_to_i64(source, function->source_ids ? function->source_ids[pc - 1] : 0);
    XR_RBC_REG(XR_RBC_A()) = (uint64_t) result;
    XR_RBC_NEXT();
}
XR_RBC_CASE(DIV)
XR_RBC_CASE(REM)
XR_RBC_CASE(REMI) {
    int64_t lhs = xr_rbc_signed(XR_RBC_REG(XR_RBC_B()));
    int64_t rhs = op == XR_RBC_REMI ?
        (XR_RBC_C() <= INT8_MAX ? (int64_t)XR_RBC_C() : (int64_t)XR_RBC_C() - 256) : xr_rbc_signed(XR_RBC_REG(XR_RBC_C()));
    if (!rhs) {
        uint32_t source_id = function->source_ids ? function->source_ids[pc - 1] : 0;
        xrt_arithmetic_panic(op == XR_RBC_DIV ? 420u : 421u, source_id);
    }
    if (lhs == INT64_MIN && rhs == -1)
        XR_RBC_REG(XR_RBC_A()) = op == XR_RBC_DIV ? (uint64_t) INT64_MIN : 0;
    else
        XR_RBC_REG(XR_RBC_A()) = (uint64_t) (op == XR_RBC_DIV ? lhs / rhs : lhs % rhs);
    XR_RBC_NEXT();
}
XR_RBC_CASE(ARITHMETIC_PANIC) {
    xrt_arithmetic_panic(instruction >> 16, XR_RBC_SOURCE());
}
XR_RBC_CASE(ZEXT) {
    // Admission restricts the width to 8, 16 or 32, so both shifts are in range.
    XR_RBC_REG(XR_RBC_A()) = XR_RBC_REG(XR_RBC_B()) & (UINT64_MAX >> (64u - XR_RBC_C()));
    XR_RBC_NEXT();
}
XR_RBC_CASE(SEXT) {
    unsigned shift = 64u - XR_RBC_C();
    XR_RBC_REG(XR_RBC_A()) = xr_rbc_shift_right(XR_RBC_REG(XR_RBC_B()) << shift, shift);
    XR_RBC_NEXT();
}
XR_RBC_CASE(AND) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) & XR_RBC_REG(XR_RBC_C()));
    XR_RBC_NEXT();
}
XR_RBC_CASE(OR) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) | XR_RBC_REG(XR_RBC_C()));
    XR_RBC_NEXT();
}
XR_RBC_CASE(XOR) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) ^ XR_RBC_REG(XR_RBC_C()));
    XR_RBC_NEXT();
}
XR_RBC_CASE(SHL) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) << (XR_RBC_REG(XR_RBC_C()) & 63));
    XR_RBC_NEXT();
}
XR_RBC_CASE(SHR) {
    XR_RBC_BINARY(xr_rbc_shift_right(XR_RBC_REG(XR_RBC_B()), XR_RBC_REG(XR_RBC_C())));
    XR_RBC_NEXT();
}
XR_RBC_CASE(USHR) {
    XR_RBC_BINARY(XR_RBC_REG(XR_RBC_B()) >> (XR_RBC_REG(XR_RBC_C()) & 63));
    XR_RBC_NEXT();
}
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
XR_RBC_CASE(LTU)
XR_RBC_CASE(LEU) {
    uint64_t lhs = (uint64_t)xr_rbc_signed(XR_RBC_REG(XR_RBC_A()));
    uint64_t rhs = (uint64_t)xr_rbc_signed(XR_RBC_REG(XR_RBC_B()));
    bool take = op == XR_RBC_LTU ? lhs < rhs : lhs <= rhs;
    instruction = code[pc++];
    if (take)
        goto xr_rbc_jump;
    XR_RBC_NEXT();
}
XR_RBC_CASE(JMP)
xr_rbc_jump: {
    uint32_t bits = instruction >> 8;
    int32_t offset = bits <= XR_RBC_JUMP_MAX ? (int32_t) bits : (int32_t) bits - 16777216;
#ifdef XR_RBC_CTFE_METERED
    if (offset < 0 && !rbc_ctfe_charge(run, XR_RBC_SOURCE())) return XR_RBC_INVALID;
#endif
    // Preparation checked the target; unsigned addition also handles backedges.
    pc += (size_t) offset;
    XR_RBC_NEXT();
}
XR_RBC_CASE(CALL) {
    uint32_t target = instruction >> 16;
    const XrRbcFunction *callee = &module->functions[target];
    size_t base = frame->base + XR_RBC_A() + 1;
#ifdef XR_RBC_CTFE_METERED
    uint32_t source = XR_RBC_SOURCE();
    if (!rbc_ctfe_charge(run, source) || !rbc_ctfe_call_reserve(run, base, callee, source))
        return XR_RBC_INVALID;
    stack->frames[stack->depth - 1].pc = pc;
#else
    frame->pc = pc;
    // Admission places the complete argument window at the caller's top.
    // Its end already fits the allocated stack, so this base cannot overflow.
    XR_DCHECK(base <= stack->slot_capacity, "call base belongs to the allocated register stack");
    if ((callee->register_count > stack->slot_capacity - base ||
         stack->depth == stack->frame_capacity ||
         callee->cell_count > stack->cell_capacity - stack->cell_count ||
         callee->spill_count > stack->spill_capacity - stack->spill_count) &&
        !xr_rbc_call_reserve(stack, base, callee)) return XR_RBC_OUT_OF_MEMORY;
#endif
    // Reserve may move every stack array. Rebind both cached pointers before
    // touching the new frame or its arguments, including on partial growth.
    frame = &stack->frames[stack->depth++];
    *frame = (XrRbcFrame){target, 0, base, XR_RBC_A(), stack->cell_count, 0, stack->spill_count, 0};
    xr_rbc_cells_begin(stack,frame->cell_base,callee->cell_count);
    registers = stack->slots + base;
#ifdef XR_RBC_CTFE_METERED
    rbc_ctfe_clear(registers, callee);
#else
    xr_rbc_entry_clear(registers, &entries[target]);
#endif
    if (callee->reference_spill_count)
        memset(stack->spills + stack->spill_count, 0, callee->reference_spill_count * sizeof(*stack->spills));
    stack->cell_count += callee->cell_count;
    stack->spill_count += callee->spill_count;
    function = callee;
    code = function->code;
    constants = function->constants;
    pc = 0;
    XR_RBC_NEXT();
}
XR_RBC_CASE(CALLN) {
    uint32_t target = instruction >> 16;
    XrRbcNativeKind kind = module->natives[target];
    XrRbcNativeShape shape = xr_rbc_native_shape(kind);
    if (!shape.window_inputs) return XR_RBC_INVALID;
    uint64_t id = XR_RBC_REG(XR_RBC_A() + shape.window_inputs);
    if (id > UINT32_MAX || id > xrt_locations_count(stack->call_locations))
        return XR_RBC_INVALID;
    switch (kind) {
    case XR_RBC_NATIVE_PRINT_I64:
        xrt_print_i64(xr_rbc_signed(XR_RBC_REG(XR_RBC_A() + 1)), (uint32_t)id); break;
    case XR_RBC_NATIVE_CLOCK_REALTIME:
        XR_RBC_REG(XR_RBC_A()) = (uint64_t)xrt_clock_realtime_nanos((uint32_t)id); break;
    case XR_RBC_NATIVE_CLOCK_CPU:
        XR_RBC_REG(XR_RBC_A()) = (uint64_t)xrt_clock_process_cpu_nanos((uint32_t)id); break;
    case XR_RBC_NATIVE_CLOCK_MONOTONIC:
        XR_RBC_REG(XR_RBC_A()) = (uint64_t)xrt_clock_monotonic_nanos((uint32_t)id); break;
    case XR_RBC_NATIVE_UTC_OFFSET_AT:
        XR_RBC_REG(XR_RBC_A()) = (uint64_t)xrt_clock_utc_offset_at(xr_rbc_signed(XR_RBC_REG(XR_RBC_A() + 1)), (uint32_t)id); break;
#define XR_MATH_RBC_INVOKE_1(name, base) \
    xrt_f64_##name(xr_rbc_float(XR_RBC_REG((base) + 1)), (uint32_t)id)
#define XR_MATH_RBC_INVOKE_2(name, base) \
    xrt_f64_##name(xr_rbc_float(XR_RBC_REG((base) + 1)), xr_rbc_float(XR_RBC_REG((base) + 2)), (uint32_t)id)
#define XR_MATH_RBC_INVOKE(tag, name, arity) \
    case XR_RBC_NATIVE_MATH_##tag: { \
        uint32_t base = XR_RBC_A(); \
        XR_RBC_REG(base) = xr_rbc_float_bits(XR_MATH_RBC_INVOKE_##arity(name, base)); \
        break; \
    }
    XR_MATH_HOST_OPERATIONS(XR_MATH_RBC_INVOKE)
#undef XR_MATH_RBC_INVOKE
#undef XR_MATH_RBC_INVOKE_2
#undef XR_MATH_RBC_INVOKE_1
    case XR_RBC_NATIVE_MATH_RANDOM:
        XR_RBC_REG(XR_RBC_A()) = xr_rbc_float_bits(xrt_math_random((uint32_t)id));
        break;
    case XR_RBC_NATIVE_MATH_RANDOM_INT: {
        uint32_t base = XR_RBC_A();
        int64_t minimum = xr_rbc_signed(XR_RBC_REG(base + 1));
        int64_t maximum = xr_rbc_signed(XR_RBC_REG(base + 2));
        XR_RBC_REG(base) = (uint64_t)xrt_math_random_int(minimum, maximum, (uint32_t)id);
        break;
    }
#if defined(XR_RBC_LINKED_MATH_LERP)
    case XR_RBC_NATIVE_MATH_LERP: {
        uint32_t base = XR_RBC_A();
        double a = xr_rbc_float(XR_RBC_REG(base + 1));
        double b = xr_rbc_float(XR_RBC_REG(base + 2));
        double t = xr_rbc_float(XR_RBC_REG(base + 3));
        XR_RBC_REG(base) = xr_rbc_float_bits(xray_stdlib_math_lerp(a, b, t));
        break;
    }
#endif
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
        value = XR_RBC_SLOT(XR_RBC_A());
        if (rbc_direct_managed_kind(function->result_kind)) XR_RBC_SLOT(XR_RBC_A()) = (XrRbcRegister){0};
    }
    uint32_t destination = frame->destination;
    stack->cell_count = frame->cell_base;
    stack->spill_count = frame->spill_base;
    if (!--stack->depth) {
        *result = value;
        return XR_RBC_OK;
    }
    --frame;
    registers = stack->slots + frame->base;
    XR_RBC_SLOT(destination) = value;
    function = &module->functions[frame->function];
    code = function->code;
    constants = function->constants;
    pc = frame->pc;
    XR_RBC_NEXT();
}
#if defined(__clang__) || defined(__GNUC__)
XR_RBC_CASE(VALUE_ACCESS) {
    XR_RBC_VALUE_EXECUTE();
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_LITERAL_BORROW) {
    XrRbcStatus status = xr_rbc_string_literal_borrow_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(STRING_LITERAL_MAP_READ) {
    uint32_t read = code[pc++];
    XrRbcStatus status = xr_rbc_map_read_step(module, stack, read >> 16, pc, instruction >> 16);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(MAP_EDIT) {
    XrRbcStatus status = xr_rbc_map_edit_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(MAP_READ) {
    XrRbcStatus status = xr_rbc_map_read_step(module, stack, instruction >> 16, pc, UINT32_MAX);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_DROP) {
    XrRbcStatus status = xr_rbc_owner_drop_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_COPY) {
    XrRbcStatus status = xr_rbc_owner_copy_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_EDGE) {
    XrRbcStatus status = xr_rbc_owner_edge_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_ARRAY_GET) {
    XrRbcStatus status = xr_rbc_array_get_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_ARRAY_LEN) {
    XrRbcStatus status = xr_rbc_array_len_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_CELL_READ) {
    XrRbcStatus status = xr_rbc_binding_copy_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_STRING_EQUAL) {
    XrRbcStatus status = xr_rbc_string_equal_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_STRING_LENGTH) {
    XrRbcStatus status = xr_rbc_string_length_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_BINDING_MOVE) {
    XR_RBC_FRAME_STEP(xr_rbc_binding_move_step);
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_ARRAY_UPDATE) {
    XrRbcStatus status = xr_rbc_array_update_step(module, stack, instruction >> 16, pc);
    if (status != XR_RBC_OK) return status;
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_CALL) {
    XR_RBC_FRAME_STEP(xr_rbc_owner_call_step);
    XR_RBC_NEXT();
}
XR_RBC_CASE(OWNER_RETURN) {
    XR_RBC_FRAME_STEP(xr_rbc_owner_return_step);
    XR_RBC_NEXT();
}
#endif
#if !defined(__clang__) && !defined(__GNUC__)
default:
    if (op == XR_RBC_VALUE_ACCESS) {
        XR_RBC_VALUE_EXECUTE();
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_STRING_LITERAL_BORROW) {
        XrRbcStatus status = xr_rbc_string_literal_borrow_step(module, stack, instruction >> 16, pc);
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_MAP_EDIT) {
        XrRbcStatus status = xr_rbc_map_edit_step(module, stack, instruction >> 16, pc);
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_OWNER_DROP || op == XR_RBC_OWNER_COPY || op == XR_RBC_OWNER_EDGE) {
        uint32_t index = instruction >> 16;
        XrRbcStatus status = op == XR_RBC_OWNER_DROP ? xr_rbc_owner_drop_step(module, stack, index, pc) :
            op == XR_RBC_OWNER_COPY ? xr_rbc_owner_copy_step(module, stack, index, pc) :
            xr_rbc_owner_edge_step(module, stack, index, pc);
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_OWNER_ARRAY_GET || op == XR_RBC_OWNER_ARRAY_LEN || op == XR_RBC_OWNER_CELL_READ) {
        uint32_t index = instruction >> 16;
        XrRbcStatus status = op == XR_RBC_OWNER_ARRAY_GET ? xr_rbc_array_get_step(module, stack, index, pc) :
            op == XR_RBC_OWNER_ARRAY_LEN ? xr_rbc_array_len_step(module, stack, index, pc) :
            xr_rbc_binding_copy_step(module, stack, index, pc);
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_OWNER_STRING_EQUAL || op == XR_RBC_OWNER_STRING_LENGTH) {
        uint32_t index = instruction >> 16;
        XrRbcStatus status = op == XR_RBC_OWNER_STRING_EQUAL ? xr_rbc_string_equal_step(module, stack, index, pc) :
            xr_rbc_string_length_step(module, stack, index, pc);
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_OWNER_BINDING_MOVE) {
        XR_RBC_FRAME_STEP(xr_rbc_binding_move_step);
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_OWNER_ARRAY_UPDATE) {
        XrRbcStatus status = xr_rbc_array_update_step(module, stack, instruction >> 16, pc);
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_OWNER_CALL) {
        XR_RBC_FRAME_STEP(xr_rbc_owner_call_step);
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_OWNER_RETURN) {
        XR_RBC_FRAME_STEP(xr_rbc_owner_return_step);
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_MAP_READ || op == XR_RBC_STRING_LITERAL_MAP_READ) {
        uint32_t literal = UINT32_MAX;
        if (op == XR_RBC_STRING_LITERAL_MAP_READ) {
            literal = instruction >> 16;
            instruction = code[pc++];
        }
        XrRbcStatus status = xr_rbc_map_read_step(module, stack, instruction >> 16, pc, literal);
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
    if (op == XR_RBC_AGGREGATE_ACCESS) {
        XR_RBC_AGGREGATE_EXECUTE();
        XR_RBC_NEXT();
    }
    if (op >= XR_RBC_MODULE_BEGIN && op <= XR_RBC_STATE_WRITE) {
        XrRbcStatus status;
        if (op >= XR_RBC_STATE_REFERENCE) {
            size_t next_pc = pc;
            status = xr_rbc_state_access(function, registers, stack->module_accesses,
                instruction, XR_RBC_SOURCE(), &next_pc);
            pc = next_pc;
        } else {
            status = xr_rbc_state_step(function, registers, stack->module_accesses,
                instruction, XR_RBC_SOURCE());
        }
        if (status != XR_RBC_OK) return status;
        XR_RBC_NEXT();
    }
}
return XR_RBC_INVALID;
#endif

#undef XR_RBC_A
#undef XR_RBC_B
#undef XR_RBC_C
#undef XR_RBC_FETCH
#undef XR_RBC_REG
#undef XR_RBC_SLOT
#undef XR_RBC_STRING
#undef XR_RBC_SOURCE
#undef XR_RBC_BINARY
#undef XR_RBC_VALUE_EXECUTE
#undef XR_RBC_FRAME_STEP
#undef XR_RBC_FRAME_RELOAD
#undef XR_RBC_AGGREGATE_EXECUTE
#undef XR_RBC_CASE
#undef XR_RBC_NEXT
