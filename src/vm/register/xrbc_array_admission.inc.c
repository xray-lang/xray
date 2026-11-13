/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_array_admission.inc.c - Exact closed Array operand and bank shapes
 */
static bool rbc_array_kind(XrRbcRegisterKind kind) {
    return xr_rbc_array_family(kind) < XR_RBC_ARRAY_FAMILY_COUNT;
}
static bool rbc_indexed_cell_kind(XrRbcRegisterKind kind) {
    return rbc_array_kind(kind) || kind == XR_RBC_MAP_STRING_I64;
}
static bool rbc_array_cell_kind(XrRbcRegisterKind kind) {
    for (uint32_t i = 0; i < XR_RBC_ARRAY_FAMILY_COUNT; ++i)
        if (kind == xr_rbc_array_cell_at(i)) return true;
    return kind == XR_RBC_MAP_STRING_I64_CELL;
}
static bool rbc_direct_managed_kind(XrRbcRegisterKind kind) {
    return kind == XR_RBC_STRING || rbc_array_kind(kind);
}
static bool rbc_managed_kind(XrRbcRegisterKind kind) {
    return kind == XR_RBC_MAP_STRING_I64 || rbc_direct_managed_kind(kind);
}
static XrRbcRegisterKind rbc_array_cell_payload(XrRbcRegisterKind kind) {
    for (uint32_t i = 0; i < XR_RBC_ARRAY_FAMILY_COUNT; ++i)
        if (kind == xr_rbc_array_cell_at(i)) return xr_rbc_array_kind_at(i);
    return kind == XR_RBC_MAP_STRING_I64_CELL ? XR_RBC_MAP_STRING_I64 : XR_RBC_BITS;
}
static XrRbcRegisterKind rbc_reference_spill_kind(const XrRbcFunction *f, uint32_t slot) {
    return f->reference_spill_kinds ? f->reference_spill_kinds[slot] : XR_RBC_STRING;
}
static XrRbcRegisterKind rbc_array_location_kind(const XrRbcFunction *f, XrRbcArrayLocation location) {
    return !location.spill ? rbc_register_kind(f, location.index) :
        location.index < f->reference_spill_count ? rbc_reference_spill_kind(f, location.index) : XR_RBC_BITS;
}
static bool rbc_array_location_valid(const XrRbcFunction *f, XrRbcArrayLocation location) {
    return location.index < (location.spill ? f->spill_count : f->register_count);
}
#include "xrbc_map_admission.inc.c"
static bool rbc_array_metadata_valid(const XrRbcFunction *f) {
    if ((f->reference_spill_kinds ? f->reference_spill_kind_count != f->reference_spill_count :
            f->reference_spill_kind_count != 0) ||
        (f->array_accesses ? f->array_access_count > 65536 : f->array_access_count != 0) ||
        f->array_operand_count > UINT32_MAX || (!f->array_operands && f->array_operand_count)) return false;
    for (uint32_t i = 0; i < f->reference_spill_count; ++i)
        if (!rbc_managed_kind(rbc_reference_spill_kind(f, i))) return false;
    for (size_t i = 0; i < f->array_operand_count; ++i)
        if (!rbc_array_location_valid(f, f->array_operands[i])) return false;
    for (size_t i = 0; i < f->array_access_count; ++i) {
        XrRbcArrayAccess access = f->array_accesses[i];
        if (access.kind == XR_RBC_MAP_STRING_I64) {
            if (!rbc_map_access_valid(f, access)) return false;
            continue;
        }
        if (!rbc_array_kind(access.kind) || access.first > f->array_operand_count ||
            access.count > f->array_operand_count - access.first ||
            (unsigned)access.operation > XR_RBC_ARRAY_DISPLAY) return false;
        uint32_t count = access.operation == XR_RBC_ARRAY_GET || access.operation == XR_RBC_ARRAY_PUSH ||
            access.operation == XR_RBC_ARRAY_CELL_WRITE || access.operation == XR_RBC_ARRAY_REPEAT ||
            access.operation == XR_RBC_ARRAY_REF_WRITE || access.operation == XR_RBC_ARRAY_REF_PUSH ? 2u :
            access.operation == XR_RBC_ARRAY_SET || access.operation == XR_RBC_ARRAY_REF_SET ? 3u : 1u;
        if (access.operation != XR_RBC_ARRAY_NEW && access.count != count) return false;
        bool result = access.operation == XR_RBC_ARRAY_NEW || access.operation == XR_RBC_ARRAY_REPEAT || access.operation == XR_RBC_ARRAY_LEN ||
            access.operation == XR_RBC_ARRAY_GET || access.operation == XR_RBC_ARRAY_RETAIN ||
            access.operation == XR_RBC_ARRAY_TAKE || access.operation == XR_RBC_ARRAY_CELL_NEW ||
            access.operation == XR_RBC_ARRAY_CELL_READ || access.operation == XR_RBC_ARRAY_BORROW ||
            access.operation == XR_RBC_ARRAY_REF_READ || access.operation == XR_RBC_ARRAY_DISPLAY;
        XrRbcRegisterKind element = access.kind == XR_RBC_ARRAY_STRING ? XR_RBC_STRING : XR_RBC_BITS;
        if (result) {
            if (!rbc_array_location_valid(f, access.result)) return false;
            XrRbcRegisterKind expected = access.operation == XR_RBC_ARRAY_DISPLAY ? XR_RBC_STRING :
                access.operation == XR_RBC_ARRAY_LEN ? XR_RBC_BITS :
                access.operation == XR_RBC_ARRAY_GET ? element : access.operation == XR_RBC_ARRAY_CELL_NEW ?
                xr_rbc_array_cell_at(xr_rbc_array_family(access.kind)) : access.operation == XR_RBC_ARRAY_BORROW ?
                xr_rbc_array_ref_at(xr_rbc_array_family(access.kind)) : access.kind;
            if (rbc_array_location_kind(f, access.result) != expected) return false;
        } else if (access.result.index || access.result.spill) return false;
        if (access.operation == XR_RBC_ARRAY_CELL_NEW) {
            if (access.cell >= f->cell_count || !f->cell_kinds || f->cell_kinds[access.cell] != access.kind) return false;
        } else if (access.cell) return false;
        for (uint32_t p = 0; p < access.count; ++p) {
            XrRbcRegisterKind expected = access.kind;
            if (access.operation == XR_RBC_ARRAY_NEW) expected = element;
            else if (access.operation == XR_RBC_ARRAY_REPEAT) expected = p ? element : XR_RBC_BITS;
            else if (p) expected = access.operation == XR_RBC_ARRAY_CELL_WRITE ||
                access.operation == XR_RBC_ARRAY_REF_WRITE ? access.kind :
                access.operation == XR_RBC_ARRAY_GET ||
                ((access.operation == XR_RBC_ARRAY_SET || access.operation == XR_RBC_ARRAY_REF_SET) && p == 1) ? XR_RBC_BITS : element;
            else if (access.operation == XR_RBC_ARRAY_SET || access.operation == XR_RBC_ARRAY_PUSH ||
                access.operation == XR_RBC_ARRAY_CELL_READ || access.operation == XR_RBC_ARRAY_CELL_WRITE ||
                access.operation == XR_RBC_ARRAY_CELL_DROP)
                expected = xr_rbc_array_cell_at(xr_rbc_array_family(access.kind));
            XrRbcArrayLocation location = f->array_operands[access.first + p];
            XrRbcRegisterKind kind = rbc_array_location_kind(f, location);
            if (!p && access.operation == XR_RBC_ARRAY_BORROW) {
                if (kind != XR_RBC_STATE_REF && kind != xr_rbc_array_cell_at(xr_rbc_array_family(access.kind))) return false;
                continue;
            }
            if (!p && (access.operation == XR_RBC_ARRAY_REF_READ || access.operation == XR_RBC_ARRAY_REF_WRITE ||
                access.operation == XR_RBC_ARRAY_REF_SET || access.operation == XR_RBC_ARRAY_REF_PUSH))
                expected = xr_rbc_array_ref_at(xr_rbc_array_family(access.kind));
            if (kind != expected && !(access.operation == XR_RBC_ARRAY_TAKE && kind == XR_RBC_CALL_WINDOW)) return false;
            if (access.operation == XR_RBC_ARRAY_TAKE && location.index == access.result.index &&
                location.spill == access.result.spill) return false;
        }
    }
    return true;
}
