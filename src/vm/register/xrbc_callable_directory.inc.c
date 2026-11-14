/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_callable_directory.inc.c - Canonical shapes below actual code targets
 */
static bool rbc_callable_canonical_type(const XrRbcModule *module, XrRbcCallableValue value,
    XrXirType *output) {
    if (value.kind == XR_RBC_CALLABLE_CELL) {
        *output = (XrXirType)value.type;
        return rbc_captured_cell_type(module, *output);
    }
    if (value.kind == XR_RBC_CALLABLE_SCALAR)
        return xrt_callable_type_id((XrtCallableValueSpec){XRT_CALLABLE_SCALAR, value.type}, 0, output);
    if (value.kind == XR_RBC_CALLABLE_FUNCTION) {
        if (value.type >= module->closure_signature_count) return false;
        *output = module->closure_signatures[value.type];
        return true;
    }
    if (value.kind != XR_RBC_CALLABLE_REFERENCE) return false;
    if (value.type == XR_RBC_STRING) { *output = XR_XIR_STRING; return true; }
    if (value.type == XR_RBC_MAP_STRING_I64)
        return xrt_callable_map_catalog(module->closure_types) &&
            xrt_callable_type_id((XrtCallableValueSpec){XRT_CALLABLE_MAP_STRING_I64, 0}, 0, output);
    uint32_t family = xr_rbc_array_family((XrRbcRegisterKind)value.type);
    return family < XR_RBC_ARRAY_FAMILY_COUNT &&
        xrt_callable_type_id((XrtCallableValueSpec){XRT_CALLABLE_ARRAY, family}, 0, output);
}

static bool rbc_callable_directory_valid(const XrRbcModule *module) {
    if (!module->closure_entry_count)
        return !module->closure_entries && !module->closure_targets &&
            !module->closure_signatures && !module->closure_signature_count;
    if (!module->closure_types || !module->closure_entries || !module->closure_targets ||
        !module->closure_signatures || !module->code_lease.owner || !module->callable_type_count ||
        module->closure_entry_count > module->function_count || module->closure_entry_count > 65536 ||
        module->closure_signature_count != module->callable_type_count) return false;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(module->closure_types);
    static const XrXirType elements[] = {XR_XIR_I64, XR_XIR_STRING, XR_XIR_U8, XR_XIR_U64};
    for (uint32_t a = 0; a < sizeof(elements) / sizeof(elements[0]); ++a)
        if (xr_xir_array_element(types, (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + a)) != elements[a]) return false;
    const XrXirTypeNode *map = xr_xir_type_node(types, XR_XIR_CONSTRUCTED_TYPE_BASE + 4u);
    if (!map || map->kind != XR_XIR_TYPE_MAP || map->element != XR_XIR_STRING || map->result != XR_XIR_I64) return false;
    for (size_t t = 0; t < module->callable_type_count; ++t) {
        XrRbcCallableType expected = module->callable_types[t];
        const XrXirTypeNode *actual = xr_xir_callable_signature(types, module->closure_signatures[t]);
        XrXirType result;
        uint32_t flags = XR_XIR_CALLABLE_ROOT_UNRESOLVED |
            (expected.flags & 1u ? XR_XIR_CALLABLE_NO_SUSPEND : 0u) |
            (expected.flags & 2u ? XR_XIR_CALLABLE_C_ABI : 0u);
        if (!actual || actual->element != XR_XIR_NEVER || actual->flags != flags ||
            actual->parameter_count != expected.parameter_count ||
            !rbc_callable_canonical_type(module, expected.result, &result) || actual->result != result) return false;
        for (uint32_t p = 0; p < expected.parameter_count; ++p) {
            XrXirType parameter;
            if (!rbc_callable_canonical_type(module, expected.parameters[p].value, &parameter) ||
                actual->parameters[p].type != parameter || actual->parameters[p].mode != expected.parameters[p].mode ||
                actual->parameters[p].escaping != expected.parameters[p].escaping) return false;
        }
    }
    for (size_t e = 0; e < module->closure_entry_count; ++e) {
        uint32_t target = module->closure_targets[e];
        if (target >= module->function_count || (e && target <= module->closure_targets[e - 1])) return false;
        const XrRbcFunction *function = &module->functions[target];
        XrtCallableEntry entry = module->closure_entries[e];
        if (!function->callable || function->state_capture || entry.capture_count != function->capture_count ||
            entry.type != module->closure_signatures[function->callable - 1] ||
            (entry.capture_count && (module->callable_types[function->callable - 1].flags & 2u)) ||
            (entry.capture_count ? !entry.captures : entry.captures != NULL)) return false;
        for (uint32_t p = 0; p < entry.capture_count; ++p) {
            XrXirType type;
            if ((function->captures[p].kind != XR_RBC_CALLABLE_SCALAR && function->captures[p].kind != XR_RBC_CALLABLE_CELL &&
                 function->captures[p].kind != XR_RBC_CALLABLE_FUNCTION &&
                 function->captures[p].kind != XR_RBC_CALLABLE_REFERENCE) ||
                !rbc_callable_canonical_type(module, function->captures[p], &type) || type != entry.captures[p]) return false;
        }
    }
    return true;
}
