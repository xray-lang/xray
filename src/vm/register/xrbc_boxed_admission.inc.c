/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_boxed_admission.inc.c - Exact canonical identities before execution
 */
static bool rbc_boxed_operation(XrRbcAggregateOperation operation) {
    return operation >= XR_RBC_BOXED_NEW && operation <= XR_RBC_BOXED_STATE_INITIALIZE;
}
static const XrXirTypeNode *rbc_boxed_type(const XrRbcModule *module, uint32_t type) {
    const XrXirTypes *types = xr_xir_compile_type_arena_types(module->closure_types);
    const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType)type);
    if (!node) return NULL;
    if (node->kind == XR_XIR_TYPE_NOMINAL && types->nominals && types->nominals->identities &&
        (types->nominals->identities[node->nominal.declaration].flags & XR_XIR_NOMINAL_RECORD)) return node;
    if (node->kind != XR_XIR_TYPE_NULLABLE) return NULL;
    const XrXirTypeNode *leaf = node;
    while (leaf && leaf->kind == XR_XIR_TYPE_NULLABLE) {
        if (leaf->element == XR_XIR_STRING) return node;
        leaf = xr_xir_type_node(types, leaf->element);
    }
    return leaf && leaf->kind == XR_XIR_TYPE_NOMINAL ? node : NULL;
}
static uint32_t rbc_boxed_register_type(const XrRbcFunction *function, uint32_t index) {
    return function->register_boxed_types && index < function->register_boxed_type_count ?
        function->register_boxed_types[index] : 0;
}
static bool rbc_boxed_function_valid(const XrRbcModule *module, const XrRbcFunction *function) {
    if (function->register_boxed_types ? function->register_boxed_type_count != function->register_count :
        function->register_boxed_type_count != 0) return false;
    for (uint32_t r = 0; r < function->register_count; ++r) {
        uint32_t type = rbc_boxed_register_type(function, r);
        if (rbc_register_kind(function, r) == XR_RBC_BOXED ? !rbc_boxed_type(module, type) : type != 0) return false;
    }
    return function->result_kind == XR_RBC_BOXED ? rbc_boxed_type(module, function->result_boxed_type) != NULL :
        !function->result_boxed_type;
}
static bool rbc_boxed_location_matches(const XrRbcModule *module, const XrRbcFunction *function,
    XrRbcArrayLocation location, XrXirType type) {
    if (!rbc_array_location_valid(function, location)) return false;
    XrRbcRegisterKind kind = rbc_array_location_kind(function, location);
    if (type == XR_XIR_STRING) return kind == XR_RBC_STRING;
    if (rbc_boxed_type(module, (uint32_t)type)) return !location.spill && kind == XR_RBC_BOXED &&
        rbc_boxed_register_type(function, location.index) == (uint32_t)type;
    return (type == XR_XIR_I64 || type == XR_XIR_BOOL || type == XR_XIR_F64) && kind == XR_RBC_BITS;
}
static bool rbc_boxed_state_type(const XrRbcModule *module, uint32_t runtime, uint32_t exact) {
    XrtModuleLayoutSpec layout;
    return module->state_layout && xrt_module_layout_view(module->state_layout, &layout) == 0 &&
        runtime < layout.type_count && layout.types[runtime].kind == XRT_MODULE_BOXED &&
        exact == XR_XIR_CONSTRUCTED_TYPE_BASE + XRT_CALLABLE_PREFIX_COUNT + layout.types[runtime].argument;
}
static bool rbc_boxed_access_valid(const XrRbcModule *module, const XrRbcFunction *function,
    XrRbcAggregateAccess access) {
    const XrXirTypeNode *type = rbc_boxed_type(module, access.slot);
    if (!type || access.first > function->aggregate_operand_count ||
        access.count > function->aggregate_operand_count - access.first || access.callee || access.window) return false;
    const XrRbcArrayLocation *args = access.count ? function->aggregate_operands + access.first : NULL;
    if (access.operation == XR_RBC_BOXED_TAKE)
        return !access.count && !access.first && !access.destination && access.source < function->register_count &&
            (rbc_register_kind(function, access.source) == XR_RBC_CALL_WINDOW ||
             rbc_boxed_register_type(function, access.source) == access.slot) &&
            rbc_boxed_location_matches(module, function, access.result, (XrXirType)access.slot);
    if (access.source) return false;
    bool state = access.operation >= XR_RBC_BOXED_STATE_READ;
    if (state) {
        bool read = access.operation == XR_RBC_BOXED_STATE_READ;
        bool construct = access.operation == XR_RBC_BOXED_STATE_CONSTRUCT;
        bool initialize = access.operation == XR_RBC_BOXED_STATE_INITIALIZE;
        if (access.count != (read ? 1u : 2u) || args[0].spill ||
            rbc_register_kind(function, args[0].index) != (construct || initialize ? XR_RBC_STATE : XR_RBC_STATE_REF)) return false;
        if (!read && !rbc_boxed_location_matches(module, function, args[1], (XrXirType)access.slot)) return false;
        if (construct || initialize) {
            XrtModuleLayoutSpec layout;
            if (!module->state_layout || xrt_module_layout_view(module->state_layout, &layout) ||
                access.destination >= layout.slot_count ||
                !rbc_boxed_state_type(module, layout.slots[access.destination].type, access.slot)) return false;
            if (construct) return !access.result.spill && access.result.index < function->register_count &&
                rbc_register_kind(function, access.result.index) == XR_RBC_STATE_REF;
        }
        return read ? rbc_boxed_location_matches(module, function, access.result, (XrXirType)access.slot) :
            !access.result.index && !access.result.spill;
    }
    if (access.operation == XR_RBC_BOXED_NEW) {
        if (access.destination || (type->kind == XR_XIR_TYPE_NULLABLE ? access.count > 1 : access.count != type->nominal.field_count)) return false;
        for (uint32_t p = 0; p < access.count; ++p)
            if (!rbc_boxed_location_matches(module, function, args[p], type->kind == XR_XIR_TYPE_NULLABLE ? type->element : type->nominal.fields[p])) return false;
        return rbc_boxed_location_matches(module, function, access.result, (XrXirType)access.slot);
    }
    if (access.count != 1 || !rbc_boxed_location_matches(module, function, args[0], (XrXirType)access.slot)) return false;
    if (access.operation == XR_RBC_BOXED_DROP) return !access.destination && !access.result.index && !access.result.spill;
    if (access.operation == XR_RBC_BOXED_RETAIN) return !access.destination &&
        rbc_boxed_location_matches(module, function, access.result, (XrXirType)access.slot);
    if (access.operation == XR_RBC_BOXED_PRESENT) return !access.destination && type->kind == XR_XIR_TYPE_NULLABLE &&
        rbc_boxed_location_matches(module, function, access.result, XR_XIR_BOOL);
    if (access.operation != XR_RBC_BOXED_FIELD || (type->kind == XR_XIR_TYPE_NULLABLE ? access.destination != 0 :
        access.destination >= type->nominal.field_count)) return false;
    return rbc_boxed_location_matches(module, function, access.result,
        type->kind == XR_XIR_TYPE_NULLABLE ? type->element : type->nominal.fields[access.destination]);
}
