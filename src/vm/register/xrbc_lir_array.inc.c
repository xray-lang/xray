/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_array.inc.c - Own typed Array roles and real operand-bank spans
 */
static uint32_t rbc_lir_array_family(XrLirType element) {
    switch (element) {
    case XR_LIR_I64: return 0;
    case XR_LIR_STRING: return 1;
    case XR_LIR_U8: return 2;
    case XR_LIR_U64: return 3;
    default: return XR_RBC_ARRAY_FAMILY_COUNT;
    }
}
static uint64_t rbc_lir_array_temporaries(const RbcLirCompiler *c, uint32_t families) {
    uint64_t count = 0;
    for (uint32_t i = 0; i < families; ++i) count += c->temporary_arrays[i];
    return count;
}
static bool rbc_lir_array_point(RbcLirCompiler *c, const XrLirInstructionView *i) {
    if (i->op == XR_LIR_CALL_INTRINSIC) {
        XrLirArrayIntrinsic shape = {0};
        return xr_lir_array_intrinsic_view(c->input, i->intrinsic, &shape) == XR_LIR_OK && shape.id;
    }
    if (i->op == XR_LIR_ARRAY_RETAIN || i->op == XR_LIR_ARRAY_DROP || i->op == XR_LIR_ARRAY_CELL_DROP ||
        i->op == XR_LIR_ARRAY_BORROW || i->op == XR_LIR_ARRAY_REF_READ || i->op == XR_LIR_ARRAY_REF_WRITE ||
        ((i->op == XR_LIR_CELL_NEW || i->op == XR_LIR_CELL_READ) &&
         (i->type == XR_LIR_ARRAY || i->type == XR_LIR_ARRAY_CELL))) return true;
    XrLirType type = XR_LIR_TYPE_INVALID;
    return i->op == XR_LIR_CELL_WRITE &&
        xr_lir_value_type(c->input, (XrLirValue){c->input,c->function.id,i->operands[0]}, &type) == XR_LIR_OK &&
        type == XR_LIR_ARRAY_CELL;
}
static XrRbcArrayLocation rbc_lir_array_location(RbcLirCompiler *c, uint32_t value) {
    bool spill = c->slots[value] == UINT32_MAX;
    return (XrRbcArrayLocation){spill ? c->spill_home[value] : c->slots[value], spill};
}
static bool rbc_lir_array_append(RbcLirCompiler *c, XrRbcArrayAccess row, const XrRbcArrayLocation *operands) {
    if (c->array_access_count > UINT16_MAX || c->array_operand_count > UINT32_MAX - row.count)
        return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "Array operation storage exceeds bytecode indices");
    void *storage = c->array_operands;
    if (!rbc_lir_grow(c, &storage, &c->array_operand_capacity,
        c->array_operand_count + row.count, sizeof(*c->array_operands))) return false;
    c->array_operands = storage;
    storage = c->array_accesses;
    if (!rbc_lir_grow(c, &storage, &c->array_access_capacity,
        c->array_access_count + 1, sizeof(*c->array_accesses))) return false;
    c->array_accesses = storage;
    row.first = (uint32_t)c->array_operand_count;
    if (row.count) memcpy(c->array_operands + row.first, operands, (size_t)row.count * sizeof(*operands));
    c->array_operand_count += row.count;
    c->array_accesses[c->array_access_count] = row;
    return rbc_lir_emit(c, XR_RBC_ARRAY_ACCESS, 0, 0, 0, (int64_t)c->array_access_count++);
}
static bool rbc_lir_array_move(RbcLirCompiler *c, XrRbcRegisterKind kind, uint32_t destination, uint32_t source) {
    if (destination == source) return true;
    if (xr_rbc_array_family(kind) < XR_RBC_ARRAY_FAMILY_COUNT) {
        XrRbcArrayLocation operand = {source, false};
        XrRbcArrayAccess row = {.operation=XR_RBC_ARRAY_TAKE, .kind=kind,
            .result={destination, false}, .count=1};
        return rbc_lir_array_append(c, row, &operand);
    }
    return rbc_lir_emit(c, kind == XR_RBC_STRING ? XR_RBC_STRING_TAKE : XR_RBC_MOV, destination, source, 0, 0);
}
static bool rbc_lir_array_emit(RbcLirCompiler *c, const XrLirInstructionView *i) {
    XrLirArrayShape shape = {0};
    XrRbcArrayOperation operation;
    if (i->op == XR_LIR_CALL_INTRINSIC) {
        XrLirArrayIntrinsic intrinsic = {0};
        if (xr_lir_array_intrinsic_view(c->input, i->intrinsic, &intrinsic) != XR_LIR_OK || !intrinsic.id)
            return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "intrinsic has no owned Array role");
        if (intrinsic.operation == XR_LIR_ARRAY_OP_SET || intrinsic.operation == XR_LIR_ARRAY_OP_PUSH) {
            XrLirType receiver = XR_LIR_TYPE_INVALID;
            xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, i->operands[0]}, &receiver);
            if (receiver != XR_LIR_ARRAY_CELL)
                return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "Array mutator receiver has no bytecode implementation");
        }
        shape.element = i->type == XR_LIR_ARRAY ? intrinsic.result_element : intrinsic.parameter_elements[0];
        switch (intrinsic.operation) {
        case XR_LIR_ARRAY_OP_NEW: operation = XR_RBC_ARRAY_NEW; break;
        case XR_LIR_ARRAY_OP_LEN: operation = XR_RBC_ARRAY_LEN; break;
        case XR_LIR_ARRAY_OP_GET: operation = XR_RBC_ARRAY_GET; break;
        case XR_LIR_ARRAY_OP_SET: operation = XR_RBC_ARRAY_SET; break;
        case XR_LIR_ARRAY_OP_PUSH: operation = XR_RBC_ARRAY_PUSH; break;
        case XR_LIR_ARRAY_OP_REPEAT: operation = XR_RBC_ARRAY_REPEAT; break;
        default: return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "unknown Array role");
        }
    } else {
        uint32_t shaped = i->result ? i->result : i->operands[0];
        if (xr_lir_array_shape_view(c->input, (XrLirValue){c->input,c->function.id,shaped}, &shape) != XR_LIR_OK)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "Array producer lost its element shape");
        switch (i->op) {
        case XR_LIR_ARRAY_RETAIN: operation = XR_RBC_ARRAY_RETAIN; break;
        case XR_LIR_ARRAY_DROP: operation = XR_RBC_ARRAY_DROP; break;
        case XR_LIR_ARRAY_CELL_DROP: operation = XR_RBC_ARRAY_CELL_DROP; break;
        case XR_LIR_CELL_NEW: operation = XR_RBC_ARRAY_CELL_NEW; break;
        case XR_LIR_CELL_READ: operation = XR_RBC_ARRAY_CELL_READ; break;
        case XR_LIR_CELL_WRITE: operation = XR_RBC_ARRAY_CELL_WRITE; break;
        case XR_LIR_ARRAY_BORROW: operation = XR_RBC_ARRAY_BORROW; break;
        case XR_LIR_ARRAY_REF_READ: operation = XR_RBC_ARRAY_REF_READ; break;
        case XR_LIR_ARRAY_REF_WRITE: operation = XR_RBC_ARRAY_REF_WRITE; break;
        default: return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "unknown Array transport");
        }
    }
    uint32_t family = rbc_lir_array_family(shape.element);
    if (family == XR_RBC_ARRAY_FAMILY_COUNT)
        return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "Array element has no bytecode implementation");
    XrRbcArrayAccess row = {.operation=operation, .kind=xr_rbc_array_kind_at(family),
        .count=i->operand_count};
    if (i->result) row.result = rbc_lir_array_location(c, i->result);
    if (operation == XR_RBC_ARRAY_CELL_NEW) {
        if (c->cell_count == 65536) return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "Array cells exceed activation capacity");
        row.cell = c->cell_count; c->cell_kinds[c->cell_count++] = row.kind;
    }
    XrRbcArrayLocation small[3], *operands = small;
    if (row.count > 3) {
        if (sizeof(*operands) > SIZE_MAX / row.count)
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "Array operand span exceeds address space");
        operands = xr_malloc((size_t)row.count * sizeof(*operands));
        if (!operands) return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot allocate Array operand span");
    }
    for (uint32_t p=0;p<row.count;++p) operands[p]=rbc_lir_array_location(c,i->operands[p]);
    bool emitted = rbc_lir_array_append(c, row, operands);
    if (operands != small) xr_free(operands);
    return emitted;
}
