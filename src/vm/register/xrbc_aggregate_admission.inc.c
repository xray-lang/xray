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

static bool rbc_aggregate_state_type(const XrRbcModule *module, uint32_t aggregate, uint32_t runtime) {
    const XrtModuleLayoutSpec *spec = module->state_spec;
    if (!spec || !spec->values || !spec->values->types) return false;
    const XrtValueImage *image = spec->values;
    /* The physical producer supplies both layouts. Match their descriptions
     * without binding a runtime plan or deriving a second field layout. */
    while (aggregate < module->aggregate_type_count && runtime && runtime <= image->type_count) {
        XrPodType type = module->aggregate_types[aggregate];
        const XrtValueImageType *state = &image->types[runtime - 1];
        if (state->owned_count || state->size != type.physical.size ||
            state->alignment != type.physical.alignment) return false;
        if (type.kind == XR_POD_FIXED_U8)
            return state->kind == XRT_VALUE_FIXED_U8 && state->count == type.count;
        if (type.kind == XR_POD_TUPLE_I64)
            return state->kind == XRT_VALUE_TUPLE_I64 && state->count == type.count;
        if (type.kind != XR_POD_NULLABLE || state->kind != XRT_VALUE_NULLABLE ||
            !state->nullable_element || state->nullable_element >= runtime || state->nullable_tag) return false;
        runtime = state->nullable_element;
        const XrtValueImageType *child = &image->types[runtime - 1];
        if (state->nullable_payload != child->alignment) return false;
        if (type.element < XR_POD_SCALAR_COUNT) {
            XrtValueKind kind;
            XrPodLayout scalar;
            return xr_pod_value_kind((XrPodScalar)type.element,&kind) && child->kind == kind &&
                !child->owned_count && xr_pod_scalar_layout((XrPodScalar)type.element,&scalar) &&
                child->size == scalar.size && child->alignment == scalar.alignment;
        }
        if (type.element - XR_POD_SCALAR_COUNT >= aggregate) return false;
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
    const XrtModuleLayoutSpec *spec = module->state_spec;
    if (!spec || !spec->slots || access.slot >= spec->slot_count || operand.spill ||
        rbc_register_kind(f, operand.index) != XR_RBC_STATE) return false;
    XrtModuleSlotSpec slot = spec->slots[access.slot];
    return rbc_aggregate_state_type(module, f->aggregate_values[home].type, slot.type) &&
        (initialize ? !(slot.flags & XRT_MODULE_SLOT_MUTABLE) :
         (slot.flags & XRT_MODULE_SLOT_MUTABLE) && rbc_aggregate_reference_location(f, access.result));
}

static bool rbc_aggregate_call_valid(const XrRbcModule *module, const XrRbcFunction *f,
    XrRbcAggregateAccess access) {
    if (access.callee >= module->function_count || access.source || access.slot ||
        access.result.index || access.result.spill || access.first > f->aggregate_argument_count ||
        access.count > f->aggregate_argument_count - access.first) return false;
    const XrRbcFunction *callee = &module->functions[access.callee];
    if (rbc_callable_inline_signature(module,access.callee)) return false;
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
        if (access.operation == XR_RBC_STATE_GROUP_INITIALIZE) {
            const XrtModuleLayoutSpec *spec = module->state_spec;
            if (access.first || access.count || access.callee || access.window ||
                access.result.index || access.result.spill || access.source >= f->register_count ||
                rbc_register_kind(f, access.source) != XR_RBC_STATE ||
                access.destination >= f->aggregate_value_count || !spec || !spec->slots ||
                !spec->values || !spec->values->types || access.slot >= spec->slot_count) return false;
            XrPodType type = module->aggregate_types[f->aggregate_values[access.destination].type];
            if (type.kind != XR_POD_TUPLE_I64 || type.count > spec->slot_count - access.slot) return false;
            uint32_t owner = spec->slots[access.slot].module;
            for (uint32_t p = 0; p < type.count; ++p) {
                XrtModuleSlotSpec slot = spec->slots[access.slot + p];
                if (slot.module != owner || !slot.type || slot.type > spec->values->type_count ||
                    spec->values->types[slot.type - 1].kind != XRT_VALUE_I64) return false;
            }
            continue;
        }
        if (access.operation == XR_RBC_RANGE_CHECK) {
            if (access.count != 3 || access.first > f->aggregate_operand_count ||
                3 > f->aggregate_operand_count - access.first || access.slot || access.source ||
                access.destination || access.callee || access.window || access.result.index || access.result.spill)
                return false;
            for (uint32_t p = 0; p < 3; ++p)
                if (!rbc_aggregate_bits_location(f, f->aggregate_operands[access.first + p])) return false;
            continue;
        }
        if (access.operation >= XR_RBC_FUNCTION_REFERENCE && access.operation <= XR_RBC_CAPTURED_CELL_BIND) {
            if (!rbc_callable_access_valid(module, f, access)) return false;
            continue;
        }
        /* Owning String results and boxed values require the common typed
         * owner homes; pure code tokens retain their separate scope checks. */
        if (access.operation == XR_RBC_STRING_LIBRARY || access.operation == XR_RBC_AGGREGATE_DISPLAY) return false;
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
        for (uint32_t p = 0; p < access.count; ++p)
            if (!rbc_aggregate_bits_location(f, f->aggregate_operands[access.first + p])) return false;
        if (access.operation == XR_RBC_AGGREGATE_NEW) {
            if (access.destination >= f->aggregate_value_count || access.source || access.result.index || access.result.spill)
                return false;
            XrRbcAggregateValue value = f->aggregate_values[access.destination];
            if ((module->aggregate_types[value.type].kind != XR_POD_FIXED_U8 &&
                 module->aggregate_types[value.type].kind != XR_POD_TUPLE_I64) || access.slot ||
                access.count != module->aggregate_types[value.type].count) return false;
        } else if (access.operation == XR_RBC_AGGREGATE_GET) {
            if (access.destination || access.source >= f->aggregate_value_count ||
                !rbc_aggregate_bits_location(f, access.result)) return false;
            XrPodType type = module->aggregate_types[f->aggregate_values[access.source].type];
            if (type.kind == XR_POD_TUPLE_I64 ? access.first || access.count || access.slot >= type.count :
                type.kind != XR_POD_FIXED_U8 || access.count != 1 || access.slot) return false;
        } else if (access.operation == XR_RBC_AGGREGATE_COPY) {
            if (access.destination >= f->aggregate_value_count || access.source >= f->aggregate_value_count ||
                f->aggregate_values[access.destination].type != f->aggregate_values[access.source].type ||
                access.first || access.count || access.slot || access.result.index || access.result.spill) return false;
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
