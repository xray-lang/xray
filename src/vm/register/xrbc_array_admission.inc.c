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
    return kind == XR_RBC_BOXED || kind == XR_RBC_MAP_STRING_I64 || rbc_direct_managed_kind(kind);
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
static bool rbc_array_metadata_valid(const XrRbcFunction *f) {
    if (f->array_access_count || f->array_accesses || f->array_operand_count || f->array_operands)
        return false;
    if (f->reference_spill_kinds ? f->reference_spill_kind_count != f->reference_spill_count :
        f->reference_spill_kind_count != 0) return false;
    for (uint32_t i = 0; i < f->reference_spill_count; ++i)
        if (!rbc_direct_managed_kind(rbc_reference_spill_kind(f,i))) return false;
    return true;
}
