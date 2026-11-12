/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_aggregate_admission.inc.c - Complete indexed aggregate storage bounds
 */
static bool rbc_aggregate_bits_location(const XrRbcFunction *f, XrRbcArrayLocation location) {
    return rbc_array_location_valid(f, location) && rbc_array_location_kind(f, location) == XR_RBC_BITS;
}

static bool rbc_aggregate_reference_location(const XrRbcFunction *f, XrRbcArrayLocation location) {
    return rbc_array_location_valid(f, location) && (location.spill ?
        location.index >= f->reference_spill_count : rbc_register_kind(f, location.index) == XR_RBC_STATE_REF);
}

static bool rbc_aggregate_state_valid(const XrRbcModule *module, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    bool read = access.operation == XR_RBC_AGGREGATE_STATE_READ;
    bool construct = access.operation == XR_RBC_AGGREGATE_STATE_CONSTRUCT;
    bool initialize = access.operation == XR_RBC_AGGREGATE_STATE_INITIALIZE;
    uint32_t home = read ? access.destination : access.source;
    if (access.count != 1 || home >= f->aggregate_value_count ||
        (read ? access.source != 0 : access.destination != 0)) return false;
    XrRbcArrayLocation operand = f->aggregate_operands[access.first];
    if (!construct && (access.result.index || access.result.spill)) return false;
    if (!construct && !initialize)
        return !access.slot && rbc_aggregate_reference_location(f, operand);
    XrtModuleLayoutSpec layout = {0};
    if (!module->state_layout || xrt_module_layout_view(module->state_layout, &layout) != 0 ||
        access.slot >= layout.slot_count || operand.spill ||
        rbc_register_kind(f, operand.index) != XR_RBC_STATE) return false;
    XrtModuleSlotSpec slot = layout.slots[access.slot];
    XrtModuleTypeSpec type = layout.types[slot.type];
    XrRbcAggregateType aggregate = module->aggregate_types[f->aggregate_values[home].type];
    return type.kind == XRT_MODULE_FIXED_U8 && type.fixed_count == aggregate.count &&
        (initialize ? !(slot.flags & XRT_MODULE_SLOT_MUTABLE) :
         (slot.flags & XRT_MODULE_SLOT_MUTABLE) && rbc_aggregate_reference_location(f, access.result));
}

static bool rbc_aggregate_metadata_valid(const XrRbcModule *module, const XrRbcFunction *f) {
    if ((!f->aggregate_values && f->aggregate_value_count) || f->aggregate_value_count > UINT32_MAX ||
        (!f->aggregate_accesses && f->aggregate_access_count) || f->aggregate_access_count > 65536 ||
        (!f->aggregate_operands && f->aggregate_operand_count) || f->aggregate_operand_count > UINT32_MAX)
        return false;
    size_t end = 0;
    for (size_t n = 0; n < f->aggregate_value_count; ++n) {
        XrRbcAggregateValue value = f->aggregate_values[n];
        if (value.type >= module->aggregate_type_count) return false;
        XrRbcAggregateLayout layout;
        if (!xr_rbc_aggregate_layout(module->aggregate_types[value.type], &layout) ||
            value.offset < end || value.offset % layout.alignment ||
            value.offset > f->aggregate_size || layout.size > f->aggregate_size - value.offset) return false;
        end = (size_t)value.offset + layout.size;
    }
    if ((!f->aggregate_value_count && f->aggregate_size) || end != f->aggregate_size) return false;
    for (size_t n = 0; n < f->aggregate_operand_count; ++n)
        if (!rbc_array_location_valid(f, f->aggregate_operands[n])) return false;
    for (size_t n = 0; n < f->aggregate_access_count; ++n) {
        XrRbcAggregateAccess access = f->aggregate_accesses[n];
        if (access.first > f->aggregate_operand_count || access.count > f->aggregate_operand_count - access.first)
            return false;
        if (access.operation >= XR_RBC_AGGREGATE_STATE_READ &&
            access.operation <= XR_RBC_AGGREGATE_STATE_INITIALIZE) {
            if (!rbc_aggregate_state_valid(module, f, access)) return false;
            continue;
        }
        if (access.slot) return false;
        for (uint32_t p = 0; p < access.count; ++p)
            if (!rbc_aggregate_bits_location(f, f->aggregate_operands[access.first + p])) return false;
        if (access.operation == XR_RBC_AGGREGATE_NEW) {
            if (access.destination >= f->aggregate_value_count || access.source || access.result.index || access.result.spill)
                return false;
            XrRbcAggregateValue value = f->aggregate_values[access.destination];
            if (access.count != module->aggregate_types[value.type].count) return false;
        } else if (access.operation == XR_RBC_AGGREGATE_GET) {
            if (access.destination || access.source >= f->aggregate_value_count || access.count != 1 ||
                !rbc_aggregate_bits_location(f, access.result)) return false;
        } else if (access.operation == XR_RBC_AGGREGATE_COPY) {
            if (access.destination >= f->aggregate_value_count || access.source >= f->aggregate_value_count ||
                f->aggregate_values[access.destination].type != f->aggregate_values[access.source].type ||
                access.first || access.count || access.result.index || access.result.spill) return false;
        } else return false;
    }
    return true;
}
