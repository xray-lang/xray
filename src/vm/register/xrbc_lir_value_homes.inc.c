/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_value_homes.inc.c - Consume sealed layouts and verified owner events
 */
typedef struct RbcHomeVector { void *data; size_t count, capacity, stride; } RbcHomeVector;
static bool rbc_home_root_append(RbcLirCompiler *c, RbcHomeVector *roots, XrLirValueLayoutKey key) {
    if (roots->count == SIZE_MAX) return rbc_lir_fail(c, XR_RBC_COMPILE_LIMIT, "value root vector count overflows");
    if (!rbc_lir_grow(c, &roots->data, &roots->capacity, roots->count + 1, sizeof(key))) return false;
    ((XrLirValueLayoutKey *)roots->data)[roots->count++] = key;
    return true;
}
static bool rbc_home_key(const XrLirModule *module, XrLirFunction function,
    uint32_t value, XrLirType type, XrLirValueLayoutKey *key) {
    bool plain = type >= XR_LIR_UNIT && type <= XR_LIR_F64;
    if (type >= XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape;
        if (xr_lir_type_view(module, type, &shape) != XR_LIR_OK) return false;
        plain = shape.kind == XR_LIR_TYPE_RECORD || shape.kind == XR_LIR_TYPE_NULLABLE ||
            shape.kind == XR_LIR_TYPE_FIXED_ARRAY || shape.kind == XR_LIR_TYPE_TUPLE;
    }
    if (!plain && type != XR_LIR_STRING && type != XR_LIR_ARRAY && type != XR_LIR_MAP_STRING_I64) return false;
    *key = (XrLirValueLayoutKey){type, 0};
    if (type == XR_LIR_ARRAY) {
        XrLirArrayShape shape = {0};
        XrLirStatus status = value ? xr_lir_array_shape_view(module,
            (XrLirValue){module, function.id, value}, &shape) : xr_lir_array_result_view(module, function, &shape);
        if (status != XR_LIR_OK) return false;
        key->array_element = shape.element;
    }
    return true;
}
static bool rbc_home_roots(RbcLirCompiler *c, RbcHomeVector *roots, bool *needed) {
    const XrLirModule *input = c->input;
    for (uint32_t f = 0; f < xr_lir_function_count(input); ++f) {
        XrLirFunction function; XrLirFunctionView view;
        if (xr_lir_function_at(input, f, &function) != XR_LIR_OK || xr_lir_function_view(input, function, &view) != XR_LIR_OK)
            return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "value root function is unreadable");
        for (size_t index = 0; index <= view.value_count; ++index) {
            uint32_t v = (uint32_t)index;
            XrLirType type = view.result; XrLirValueLayoutKey key;
            if (v && xr_lir_value_type(input, (XrLirValue){input, function.id, v}, &type) != XR_LIR_OK)
                return rbc_lir_fail(c, XR_RBC_COMPILE_INVALID, "value root is unreadable");
            if (!rbc_home_key(input, function, v, type, &key)) continue;
            *needed |= type > XR_LIR_F64;
            const XrLirValueLayoutKey *keys = roots->data;
            size_t i = 0;
            while (i < roots->count && (keys[i].type != key.type || keys[i].array_element != key.array_element)) ++i;
            if (i == roots->count && !rbc_home_root_append(c, roots, key)) return false;
        }
    }
    return true;
}
static XrRbcCompileStatus rbc_home_layout_status(XrLirValueLayoutStatus status) {
    switch (status) {
    case XR_LIR_VALUE_LAYOUT_OK: return XR_RBC_COMPILE_OK;
    case XR_LIR_VALUE_LAYOUT_OOM: return XR_RBC_COMPILE_OUT_OF_MEMORY;
    case XR_LIR_VALUE_LAYOUT_LIMIT: case XR_LIR_VALUE_LAYOUT_OVERFLOW: return XR_RBC_COMPILE_LIMIT;
    case XR_LIR_VALUE_LAYOUT_UNSUPPORTED: return XR_RBC_COMPILE_UNSUPPORTED;
    default: return XR_RBC_COMPILE_INVALID;
    }
}
static XrRbcCompileStatus rbc_lir_value_homes(const XrLirModule *input, const uint32_t *ids,
    XrRbcProgram *program, XrRbcCompileDiagnostic *diagnostic) {
    bool typed = false;
    for (size_t f = 0; f < program->module.function_count; ++f) {
        const XrRbcFunction *function = &program->module.functions[f];
        typed |= function->register_kinds != NULL || function->aggregate_value_count != 0 || function->result_kind != XR_RBC_BITS;
    }
    if (!typed) return XR_RBC_COMPILE_OK;
    RbcLirCompiler c = {.input=input, .diagnostic=diagnostic};
    RbcHomeVector roots = {.stride=sizeof(XrLirValueLayoutKey)};
    XrtBillingOwner *billing = NULL; XrLirValueLayouts *layouts = NULL; XrLirOwnershipPlan *ownership = NULL;
    bool needed = false;
    if (!rbc_home_roots(&c, &roots, &needed)) { xr_free(roots.data); return c.status; }
    if (!needed) { xr_free(roots.data); return XR_RBC_COMPILE_OK; }
    XrtManagedStatus memory = xrt_billing_owner_new(NULL, SIZE_MAX, &billing);
    XrRbcCompileStatus status = memory == XRT_MANAGED_OK ? XR_RBC_COMPILE_OK :
        memory == XRT_MANAGED_OOM ? XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_LIMIT;
    if (status == XR_RBC_COMPILE_OK)
        status = rbc_home_layout_status(xr_lir_value_layouts_create(input, roots.data, roots.count, billing, &layouts));
    if (status == XR_RBC_COMPILE_OK) {
        const XrLirOwnershipTypeFact *types = NULL; uint32_t count = 0;
        if (xr_lir_value_layouts_ownership(layouts, &types, &count) != XR_LIR_VALUE_LAYOUT_OK) status = XR_RBC_COMPILE_INVALID;
        else {
            XrLirDiagnostic invalid = {0};
            XrLirStatus verified = xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){input, types, count}, &ownership, &invalid);
            if (verified != XR_LIR_OK) {
                status = verified == XR_LIR_OUT_OF_MEMORY ? XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_INVALID;
                diagnostic->function = invalid.function; diagnostic->block = invalid.block; diagnostic->instruction = invalid.instruction;
                rbc_lir_error(diagnostic, status, invalid.message, invalid.source);
            } else {
                for (size_t f = 0; f < program->module.function_count && status == XR_RBC_COMPILE_OK; ++f) {
                    XrLirValueHomes *homes = NULL;
                    XrLirValueHomesRequest request = {input, {input, ids[f]}, layouts, ownership};
                    status = rbc_home_layout_status(xr_lir_value_homes_create(&request, &homes));
                    if (status == XR_RBC_COMPILE_OK) ((XrRbcFunction *)program->module.functions)[f].value_homes = homes;
                    else diagnostic->function = ids[f];
                }
            }
        }
    }
    if (status != XR_RBC_COMPILE_OK && diagnostic->status == XR_RBC_COMPILE_OK)
        rbc_lir_error(diagnostic, status, "cannot project receiving value homes", (XrLirSource){0});
    xr_lir_ownership_plan_destroy(ownership); xr_lir_value_layouts_destroy(layouts);
    xrt_billing_owner_drop(billing); xr_free(roots.data);
    return status;
}
