/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_string_library_admission.inc.c - Exact String roles and borrowed identities
 */
static bool rbc_string_library_valid(const XrRbcFunction *f, XrRbcAggregateAccess access) {
    static const XrRbcRegisterKind first[] = {XR_RBC_STRING, XR_RBC_STRING_CELL, XR_RBC_STATE_REF,
        XR_RBC_STRING_CELL, XR_RBC_STATE_REF, XR_RBC_STRING_REF, XR_RBC_STRING_REF, XR_RBC_STRING_REF};
    static const XrRbcRegisterKind results[] = {XR_RBC_STRING, XR_RBC_BITS, XR_RBC_BITS,
        XR_RBC_STRING_REF, XR_RBC_STRING_REF, XR_RBC_STRING, XR_RBC_BITS, XR_RBC_BITS};
    if (access.slot < XR_LIR_STRING_OP_APPEND_OWNED || access.slot >= XR_LIR_STRING_OP_END ||
        access.destination || access.source || access.callee || access.window) return false;
    uint32_t index = access.slot - XR_LIR_STRING_OP_APPEND_OWNED;
    uint32_t count = access.slot >= XR_LIR_STRING_OP_BORROW_CELL && access.slot <= XR_LIR_STRING_OP_READ_REF ? 1u : 2u;
    if (access.count != count || access.first > f->aggregate_operand_count ||
        count > f->aggregate_operand_count - access.first ||
        rbc_array_location_kind(f, f->aggregate_operands[access.first]) != first[index]) return false;
    if (count == 2 && rbc_array_location_kind(f, f->aggregate_operands[access.first + 1]) != XR_RBC_STRING)
        return false;
    return results[index] == XR_RBC_BITS ? !access.result.index && !access.result.spill :
        rbc_array_location_valid(f, access.result) && rbc_array_location_kind(f, access.result) == results[index];
}
