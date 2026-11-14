/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_map.inc.c - Closed Map roles in the existing typed operand owner
 *
 * KEY CONCEPT:
 *   Map values use the existing typed locations and owned operand directory.
 *   State access stays in the cold handler with exact payload metadata.
 */
static uint32_t rbc_lir_map_role(const XrLirModule *module, const XrLirInstructionView *i) {
    XrLirIntrinsic entry = {0};
    return i->op == XR_LIR_CALL_INTRINSIC &&
        xr_lir_intrinsic_view(module, i->intrinsic, &entry) == XR_LIR_OK &&
        xr_lir_map_intrinsic_valid(module, &entry) ? entry.operation : 0;
}

static bool rbc_lir_map_point(RbcLirCompiler *c, const XrLirInstructionView *i) {
    if (rbc_lir_captured_cell_point(c->input, c->function, i)) return false;
    if (rbc_lir_map_role(c->input, i)) return true;
    if (i->op == XR_LIR_STATE_READ) return i->type == XR_LIR_MAP_STRING_I64;
    if (i->op == XR_LIR_STATE_WRITE) {
        XrLirType type = XR_LIR_TYPE_INVALID;
        return xr_lir_value_type(c->input, (XrLirValue){c->input, c->function.id, i->operands[1]}, &type) == XR_LIR_OK &&
            type == XR_LIR_MAP_STRING_I64;
    }
    if (i->op >= XR_LIR_MAP_RETAIN && i->op <= XR_LIR_MAP_REF_WRITE) return true;
    if (i->op == XR_LIR_CELL_NEW) return i->type == XR_LIR_MAP_STRING_I64_CELL;
    if (i->op == XR_LIR_CELL_READ) return i->type == XR_LIR_MAP_STRING_I64;
    XrLirType type = XR_LIR_TYPE_INVALID;
    return i->op == XR_LIR_CELL_WRITE && xr_lir_value_type(c->input,
        (XrLirValue){c->input, c->function.id, i->operands[0]}, &type) == XR_LIR_OK &&
        type == XR_LIR_MAP_STRING_I64_CELL;
}

static bool rbc_lir_map_emit(RbcLirCompiler *c, const XrLirInstructionView *i) {
    XrRbcArrayOperation operation;
    switch (rbc_lir_map_role(c->input, i)) {
    case XR_LIR_MAP_OP_NEW: operation = XR_RBC_ARRAY_NEW; break;
    case XR_LIR_MAP_OP_LEN: operation = XR_RBC_ARRAY_LEN; break;
    case XR_LIR_MAP_OP_GET: operation = XR_RBC_ARRAY_GET; break;
    case XR_LIR_MAP_OP_CONTAINS: operation = XR_RBC_MAP_CONTAINS; break;
    case XR_LIR_MAP_OP_SET: operation = XR_RBC_ARRAY_SET; break;
    case XR_LIR_MAP_OP_REMOVE: operation = XR_RBC_MAP_REMOVE; break;
    case XR_LIR_MAP_OP_CLEAR: operation = XR_RBC_MAP_CLEAR; break;
    default:
        switch (i->op) {
        case XR_LIR_STATE_READ: operation = XR_RBC_MAP_STATE_READ; break;
        case XR_LIR_STATE_WRITE: operation = XR_RBC_MAP_STATE_WRITE; break;
        case XR_LIR_MAP_RETAIN: operation = XR_RBC_ARRAY_RETAIN; break;
        case XR_LIR_MAP_DROP: operation = XR_RBC_ARRAY_DROP; break;
        case XR_LIR_MAP_CELL_DROP: operation = XR_RBC_ARRAY_CELL_DROP; break;
        case XR_LIR_MAP_BORROW: operation = XR_RBC_ARRAY_BORROW; break;
        case XR_LIR_MAP_REF_READ: operation = XR_RBC_ARRAY_REF_READ; break;
        case XR_LIR_MAP_REF_WRITE: operation = XR_RBC_ARRAY_REF_WRITE; break;
        case XR_LIR_CELL_NEW: operation = XR_RBC_ARRAY_CELL_NEW; break;
        case XR_LIR_CELL_READ: operation = XR_RBC_ARRAY_CELL_READ; break;
        case XR_LIR_CELL_WRITE: operation = XR_RBC_ARRAY_CELL_WRITE; break;
        default: return rbc_lir_fail(c, XR_RBC_COMPILE_UNSUPPORTED, "Map operation has no bytecode role");
        }
    }
    XrLirType receiver = XR_LIR_TYPE_INVALID;
    if (i->operand_count) xr_lir_value_type(c->input,
        (XrLirValue){c->input, c->function.id, i->operands[0]}, &receiver);
    if (receiver == XR_LIR_MAP_STRING_I64_REF) {
        if (operation == XR_RBC_ARRAY_SET) operation = XR_RBC_ARRAY_REF_SET;
        if (operation == XR_RBC_MAP_REMOVE) operation = XR_RBC_MAP_REF_REMOVE;
        if (operation == XR_RBC_MAP_CLEAR) operation = XR_RBC_MAP_REF_CLEAR;
    }
    XrRbcArrayAccess row = {.operation=operation, .kind=XR_RBC_MAP_STRING_I64, .count=i->operand_count};
    if (i->result) row.result = operation == XR_RBC_ARRAY_GET ?
        (XrRbcArrayLocation){c->aggregate_home[i->result], false} : rbc_lir_array_location(c, i->result);
    if (operation == XR_RBC_ARRAY_CELL_NEW) {
        if (c->cell_count == 65536)
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "Map cells exceed activation capacity");
        row.cell = c->cell_count;
        c->cell_kinds[c->cell_count++] = XR_RBC_MAP_STRING_I64;
    }
    if (operation == XR_RBC_ARRAY_BORROW && rbc_lir_captured_cell(c->input, receiver))
        row.cell = XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT + receiver - XR_LIR_TYPE_COUNT;
    XrRbcArrayLocation small[3], *operands = small;
    if (row.count > 3) {
        if (sizeof(*operands) > SIZE_MAX / row.count)
            return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "Map operands exceed address space");
        operands = xr_malloc((size_t)row.count * sizeof(*operands));
        if (!operands) return rbc_lir_fail(c, XR_RBC_COMPILE_OUT_OF_MEMORY, "cannot own Map operands");
    }
    for (uint32_t p = 0; p < row.count; ++p) operands[p] = rbc_lir_array_location(c, i->operands[p]);
    bool emitted = rbc_lir_array_append(c, row, operands);
    if (operands != small) xr_free(operands);
    return emitted;
}
