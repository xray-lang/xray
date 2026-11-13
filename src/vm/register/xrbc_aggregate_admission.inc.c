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

static bool rbc_display_valid(const XrRbcModule *module, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    if (access.destination || access.callee || access.window || !rbc_array_location_valid(f, access.result) ||
        rbc_array_location_kind(f, access.result) != XR_RBC_STRING) return false;
    if (access.slot == XR_POD_BOOL || access.slot == XR_POD_U8 || access.slot == XR_POD_U64 ||
        access.slot == XR_POD_I64 || access.slot == XR_POD_F64)
        return !access.source && access.count == 1 && access.first < f->aggregate_operand_count &&
            rbc_aggregate_bits_location(f, f->aggregate_operands[access.first]);
    if (access.slot < XR_POD_SCALAR_COUNT || access.source >= f->aggregate_value_count ||
        access.slot - XR_POD_SCALAR_COUNT != f->aggregate_values[access.source].type ||
        access.first || access.count) return false;
    uint32_t type = access.slot;
    while (type >= XR_POD_SCALAR_COUNT) {
        uint32_t index = type - XR_POD_SCALAR_COUNT;
        if (index >= module->aggregate_type_count || module->aggregate_types[index].kind != XR_POD_NULLABLE)
            return false;
        type = module->aggregate_types[index].element;
    }
    return type < XR_POD_SCALAR_COUNT;
}

static bool rbc_aggregate_reference_location(const XrRbcFunction *f, XrRbcArrayLocation location) {
    return rbc_array_location_valid(f, location) && (location.spill ?
        location.index >= f->reference_spill_count : rbc_register_kind(f, location.index) == XR_RBC_STATE_REF);
}

static bool rbc_aggregate_state_type(const XrRbcModule *module, uint32_t aggregate, uint32_t runtime) {
    XrtModuleLayoutSpec layout = {0};
    if (!module->state_layout || xrt_module_layout_view(module->state_layout, &layout) != 0) return false;
    while (aggregate < module->aggregate_type_count && runtime < layout.type_count) {
        XrPodType type = module->aggregate_types[aggregate];
        XrtModuleTypeSpec state = layout.types[runtime];
        if (type.kind == XR_POD_FIXED_U8)
            return state.kind == XRT_MODULE_FIXED_U8 && state.argument == type.count;
        if (state.kind != XRT_MODULE_NULLABLE || state.argument >= runtime) return false;
        runtime = state.argument;
        if (type.element < XR_POD_SCALAR_COUNT)
            return layout.types[runtime].kind == xr_pod_module_kind((XrPodScalar)type.element);
        aggregate = type.element - XR_POD_SCALAR_COUNT;
    }
    return false;
}

static bool rbc_nullable_access_valid(const XrRbcModule *module, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    if (access.first || access.count || access.slot || access.callee || access.window) return false;
    bool create = access.operation == XR_RBC_NULLABLE_NONE || access.operation == XR_RBC_NULLABLE_SOME;
    uint32_t home = create ? access.destination : access.source;
    if (home >= f->aggregate_value_count) return false;
    XrPodType type = module->aggregate_types[f->aggregate_values[home].type];
    if (type.kind != XR_POD_NULLABLE) return false;
    if (access.operation == XR_RBC_NULLABLE_NONE)
        return !access.source && !access.result.index && !access.result.spill;
    if (access.operation == XR_RBC_NULLABLE_IS_SOME)
        return !access.destination && rbc_aggregate_bits_location(f, access.result);
    if (type.element >= XR_POD_SCALAR_COUNT) {
        uint32_t payload = create ? access.source : access.destination;
        return !access.result.index && !access.result.spill && payload < f->aggregate_value_count &&
            f->aggregate_values[payload].type == type.element - XR_POD_SCALAR_COUNT;
    }
    if (create ? access.source != 0 : access.destination != 0) return false;
    return type.element == XR_POD_UNIT ? !access.result.index && !access.result.spill :
        rbc_aggregate_bits_location(f, access.result);
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
    return rbc_aggregate_state_type(module, f->aggregate_values[home].type, slot.type) &&
        (initialize ? !(slot.flags & XRT_MODULE_SLOT_MUTABLE) :
         (slot.flags & XRT_MODULE_SLOT_MUTABLE) && rbc_aggregate_reference_location(f, access.result));
}

static bool rbc_aggregate_return_word(const XrRbcFunction *f, uint32_t word) {
    return (word & 255) == XR_RBC_AGGREGATE_ACCESS && (word >> 16) < f->aggregate_access_count &&
        (f->aggregate_accesses[word >> 16].operation == XR_RBC_AGGREGATE_RETURN ||
         f->aggregate_accesses[word >> 16].operation == XR_RBC_FUNCTION_RETURN);
}

static bool rbc_aggregate_call_valid(const XrRbcModule *module, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    if (access.callee >= module->function_count || access.source || access.slot ||
        access.result.index || access.result.spill || access.first > f->aggregate_argument_count ||
        access.count > f->aggregate_argument_count - access.first) return false;
    const XrRbcFunction *callee = &module->functions[access.callee];
    if ((!callee->aggregate_parameter_count && !callee->aggregate_result_type && !callee->callable) ||
        access.count != callee->aggregate_parameter_count || access.window >= f->register_count ||
        callee->parameter_count != f->register_count - access.window - 1 ||
        rbc_register_kind(f, access.window) != XR_RBC_CALL_WINDOW) return false;
    for (uint32_t n = 0; n < access.count; ++n) {
        uint32_t actual = f->aggregate_arguments[access.first + n];
        uint32_t formal = callee->aggregate_parameters[n];
        if (actual >= f->aggregate_value_count ||
            f->aggregate_values[actual].type != callee->aggregate_values[formal].type) return false;
    }
    return callee->aggregate_result_type ? access.destination < f->aggregate_value_count &&
        f->aggregate_values[access.destination].type == callee->aggregate_result_type - 1 : !access.destination;
}

static bool rbc_aggregate_signature_valid(const XrRbcModule *module, const XrRbcFunction *f) {
    if ((!f->aggregate_parameters && f->aggregate_parameter_count) ||
        f->aggregate_parameter_count > f->aggregate_value_count ||
        (!f->aggregate_arguments && f->aggregate_argument_count) || f->aggregate_argument_count > UINT32_MAX ||
        f->aggregate_result_type > module->aggregate_type_count ||
        (f->aggregate_result_type && f->result_kind != XR_RBC_BITS)) return false;
    for (size_t n = 0; n < f->aggregate_parameter_count; ++n)
        if (f->aggregate_parameters[n] >= f->aggregate_value_count ||
            (n && f->aggregate_parameters[n] <= f->aggregate_parameters[n - 1])) return false;
    for (size_t n = 0; n < f->aggregate_argument_count; ++n)
        if (f->aggregate_arguments[n] >= f->aggregate_value_count) return false;
    return true;
}

#include "xrbc_string_library_admission.inc.c"

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
    if (!rbc_aggregate_signature_valid(module, f)) return false;
    for (size_t n = 0; n < f->aggregate_operand_count; ++n)
        if (!rbc_array_location_valid(f, f->aggregate_operands[n])) return false;
    for (size_t n = 0; n < f->aggregate_access_count; ++n) {
        XrRbcAggregateAccess access = f->aggregate_accesses[n];
        if (access.operation >= XR_RBC_FUNCTION_REFERENCE && access.operation <= XR_RBC_FUNCTION_RETURN) {
            if (!rbc_callable_access_valid(module, f, access)) return false;
            continue;
        }
        if (access.operation == XR_RBC_STRING_LIBRARY) {
            if (!rbc_string_library_valid(f, access)) return false;
            continue;
        }
        if (access.operation == XR_RBC_AGGREGATE_DISPLAY) {
            if (!rbc_display_valid(module, f, access)) return false;
            continue;
        }
        if (access.operation >= XR_RBC_NULLABLE_NONE && access.operation <= XR_RBC_NULLABLE_UNWRAP) {
            if (!rbc_nullable_access_valid(module, f, access)) return false;
            continue;
        }
        if (access.operation == XR_RBC_AGGREGATE_CALL) continue;
        if (access.callee || access.window) return false;
        if (access.operation == XR_RBC_AGGREGATE_RETURN) {
            if (!f->aggregate_result_type || access.source >= f->aggregate_value_count ||
                f->aggregate_values[access.source].type != f->aggregate_result_type - 1 ||
                access.destination || access.first || access.count || access.slot || access.result.index || access.result.spill)
                return false;
            continue;
        }
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
            if (module->aggregate_types[value.type].kind != XR_POD_FIXED_U8 ||
                access.count != module->aggregate_types[value.type].count) return false;
        } else if (access.operation == XR_RBC_AGGREGATE_GET) {
            if (access.destination || access.source >= f->aggregate_value_count || access.count != 1 ||
                !rbc_aggregate_bits_location(f, access.result) ||
                module->aggregate_types[f->aggregate_values[access.source].type].kind != XR_POD_FIXED_U8) return false;
        } else if (access.operation == XR_RBC_AGGREGATE_COPY) {
            if (access.destination >= f->aggregate_value_count || access.source >= f->aggregate_value_count ||
                f->aggregate_values[access.destination].type != f->aggregate_values[access.source].type ||
                access.first || access.count || access.result.index || access.result.spill) return false;
        } else return false;
    }
    return true;
}

static bool rbc_aggregate_calls_valid(const XrRbcModule *module, const XrRbcFunction *f) {
    for (size_t n = 0; n < f->aggregate_access_count; ++n)
        if (f->aggregate_accesses[n].operation == XR_RBC_AGGREGATE_CALL &&
            !rbc_aggregate_call_valid(module, f, f->aggregate_accesses[n])) return false;
    return true;
}
