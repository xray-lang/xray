/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_map_admission.inc.c - Exact Map locations and operation shapes
 *
 * KEY CONCEPT:
 *   The receiving function validates each Map operation against its actual
 *   register, spill, Cell and Nullable result directories before execution.
 */
static bool rbc_map_access_valid(const XrRbcFunction *f, XrRbcArrayAccess a) {
    if (a.first > f->array_operand_count || a.count > f->array_operand_count - a.first) return false;
    uint32_t count = 1;
    XrRbcRegisterKind result = XR_RBC_MAP_STRING_I64, receiver = XR_RBC_MAP_STRING_I64;
    bool output = true;
    switch (a.operation) {
    case XR_RBC_ARRAY_NEW: if (a.count % 2) return false; count = a.count; break;
    case XR_RBC_ARRAY_LEN: result = XR_RBC_BITS; break;
    case XR_RBC_ARRAY_GET: count = 2; break;
    case XR_RBC_MAP_CONTAINS: count = 2; result = XR_RBC_BITS; break;
    case XR_RBC_ARRAY_RETAIN: case XR_RBC_ARRAY_TAKE: break;
    case XR_RBC_MAP_STATE_READ: receiver = XR_RBC_STATE_REF; break;
    case XR_RBC_MAP_STATE_WRITE: count = 2; receiver = XR_RBC_STATE_REF; output = false; break;
    case XR_RBC_ARRAY_DROP: output = false; break;
    case XR_RBC_ARRAY_CELL_NEW: result = XR_RBC_MAP_STRING_I64_CELL; break;
    case XR_RBC_ARRAY_CELL_READ: receiver = XR_RBC_MAP_STRING_I64_CELL; break;
    case XR_RBC_ARRAY_CELL_WRITE: count = 2; receiver = XR_RBC_MAP_STRING_I64_CELL; output = false; break;
    case XR_RBC_ARRAY_CELL_DROP: receiver = XR_RBC_MAP_STRING_I64_CELL; output = false; break;
    case XR_RBC_ARRAY_SET: count = 3; receiver = XR_RBC_MAP_STRING_I64_CELL; output = false; break;
    case XR_RBC_MAP_REMOVE: count = 2; receiver = XR_RBC_MAP_STRING_I64_CELL; result = XR_RBC_BITS; break;
    case XR_RBC_MAP_CLEAR: receiver = XR_RBC_MAP_STRING_I64_CELL; output = false; break;
    case XR_RBC_ARRAY_BORROW: result = XR_RBC_MAP_STRING_I64_REF; break;
    case XR_RBC_ARRAY_REF_READ: receiver = XR_RBC_MAP_STRING_I64_REF; break;
    case XR_RBC_ARRAY_REF_WRITE: count = 2; receiver = XR_RBC_MAP_STRING_I64_REF; output = false; break;
    case XR_RBC_ARRAY_REF_SET: count = 3; receiver = XR_RBC_MAP_STRING_I64_REF; output = false; break;
    case XR_RBC_MAP_REF_REMOVE: count = 2; receiver = XR_RBC_MAP_STRING_I64_REF; result = XR_RBC_BITS; break;
    case XR_RBC_MAP_REF_CLEAR: receiver = XR_RBC_MAP_STRING_I64_REF; output = false; break;
    default: return false;
    }
    if (a.count != count) return false;
    if (a.operation == XR_RBC_ARRAY_GET) {
        if (a.result.spill || !f->aggregate_values || a.result.index >= f->aggregate_value_count) return false;
    } else if (output) {
        if (!rbc_array_location_valid(f, a.result) || rbc_array_location_kind(f, a.result) != result) return false;
        if (result == XR_RBC_MAP_STRING_I64_REF && a.result.spill) return false;
    } else if (a.result.index || a.result.spill) return false;
    if (a.operation == XR_RBC_ARRAY_CELL_NEW) {
        if (!f->cell_kinds || a.cell >= f->cell_count || f->cell_kinds[a.cell] != XR_RBC_MAP_STRING_I64) return false;
    } else if (a.cell) return false;
    for (uint32_t p = 0; p < a.count; ++p) {
        XrRbcArrayLocation location = f->array_operands[a.first + p];
        XrRbcRegisterKind kind = rbc_array_location_kind(f, location), expected = receiver;
        if (a.operation == XR_RBC_ARRAY_NEW) expected = p % 2 ? XR_RBC_BITS : XR_RBC_STRING;
        else if (p) expected = a.operation == XR_RBC_ARRAY_CELL_WRITE || a.operation == XR_RBC_ARRAY_REF_WRITE ||
            a.operation == XR_RBC_MAP_STATE_WRITE ?
            XR_RBC_MAP_STRING_I64 : p == 1 ? XR_RBC_STRING : XR_RBC_BITS;
        else if (a.operation == XR_RBC_ARRAY_BORROW) {
            if (location.spill || (kind != XR_RBC_MAP_STRING_I64_CELL && kind != XR_RBC_STATE_REF)) return false;
            continue;
        }
        if (!p && receiver == XR_RBC_STATE_REF && location.spill && location.index >= f->reference_spill_count)
            continue;
        if (kind != expected && !(a.operation == XR_RBC_ARRAY_TAKE && kind == XR_RBC_CALL_WINDOW)) return false;
        if (a.operation == XR_RBC_ARRAY_TAKE && location.index == a.result.index && location.spill == a.result.spill)
            return false;
    }
    return true;
}

static bool rbc_map_module_valid(const XrRbcModule *module, const XrRbcFunction *f) {
    for (size_t n = 0; n < f->array_access_count; ++n) {
        XrRbcArrayAccess a = f->array_accesses[n];
        if (a.kind != XR_RBC_MAP_STRING_I64 || a.operation != XR_RBC_ARRAY_GET) continue;
        uint32_t type = f->aggregate_values[a.result.index].type;
        if (type >= module->aggregate_type_count || module->aggregate_types[type].kind != XR_POD_NULLABLE ||
            module->aggregate_types[type].element != XR_POD_I64) return false;
    }
    return true;
}
