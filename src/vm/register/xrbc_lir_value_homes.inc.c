/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_value_homes.inc.c - One cold receiving preparation for immutable LIR
 */
typedef struct RbcHomeVector { void *data; size_t count, capacity; } RbcHomeVector;
typedef struct RbcValuePreparation {
    XrtBillingOwner *billing;
    XrLirValueLayouts *layouts;
    XrLirOwnershipPlan *ownership;
} RbcValuePreparation;
static bool rbc_home_key(const XrLirModule *module, XrLirFunction function,
    uint32_t value, XrLirType type, XrLirValueLayoutKey *key) {
    if (value && xr_lir_string_binding(module,(XrLirValue){module,function.id,value})) type=XR_LIR_STRING;
    if (type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF) type = XR_LIR_ARRAY;
    if (type == XR_LIR_MAP_STRING_I64_CELL || type == XR_LIR_MAP_STRING_I64_REF) type = XR_LIR_MAP_STRING_I64;
    bool plain = type >= XR_LIR_UNIT && type <= XR_LIR_F64;
    if (type >= XR_LIR_TYPE_COUNT) {
        XrLirTypeView shape;
        if (xr_lir_type_view(module,type,&shape) != XR_LIR_OK) return false;
        plain = shape.kind == XR_LIR_TYPE_CALLABLE || rbc_lir_captured_cell(module,type) || shape.kind == XR_LIR_TYPE_SLICE || shape.kind == XR_LIR_TYPE_RECORD || shape.kind == XR_LIR_TYPE_NULLABLE ||
            shape.kind == XR_LIR_TYPE_FIXED_ARRAY || shape.kind == XR_LIR_TYPE_TUPLE;
    }
    if (!plain && type != XR_LIR_STRING && type != XR_LIR_ARRAY && type != XR_LIR_MAP_STRING_I64) return false;
    *key = (XrLirValueLayoutKey){type,0,0};
    if (type == XR_LIR_ARRAY) {
        XrLirArrayShape shape = {0};
        XrLirStatus status = value ? xr_lir_array_shape_view(module,
            (XrLirValue){module,function.id,value},&shape) : xr_lir_array_result_view(module,function,&shape);
        if (status != XR_LIR_OK) return false;
        key->array_element = shape.element;
    }
    return true;
}
static bool rbc_home_needed(const XrLirModule *input, const uint32_t *ids, uint32_t count) {
    for (uint32_t f = 0; f < count; ++f) {
        XrLirFunction function = {input,ids[f]}; XrLirFunctionView view;
        xr_lir_function_view(input,function,&view);
        for (size_t index = 0; index <= view.value_count; ++index) {
            XrLirType type = view.result; XrLirValueLayoutKey key;
            if (index) xr_lir_value_type(input,(XrLirValue){input,function.id,(uint32_t)index},&type);
            if (type > XR_LIR_F64 && rbc_home_key(input,function,(uint32_t)index,type,&key)) return true;
        }
    }
    return false;
}
static bool rbc_home_root_append(RbcLirCompiler *c, RbcHomeVector *roots, uint8_t *seen,
    size_t array_base, XrLirValueLayoutKey key) {
    size_t index = key.type;
    if (key.environment_function) {
        if (key.environment_function > xr_lir_function_count(c->input))
            return rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"environment root has no exact target");
        index = array_base + XR_RBC_ARRAY_FAMILY_COUNT + key.environment_function - 1;
    } else if (key.type == XR_LIR_ARRAY) {
        uint32_t family = rbc_lir_array_family(key.array_element);
        if (family >= XR_RBC_ARRAY_FAMILY_COUNT)
            return rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"value root Array element is unavailable");
        index = array_base + family;
    }
    if (seen[index]) return true;
    if (roots->count == SIZE_MAX) return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"value root count overflows");
    if (!rbc_lir_grow(c,&roots->data,&roots->capacity,roots->count + 1,sizeof(key))) return false;
    ((XrLirValueLayoutKey *)roots->data)[roots->count++] = key;
    seen[index] = 1;
    return true;
}
static bool rbc_home_roots(RbcLirCompiler *c, const XrLirStateLayout *state, RbcHomeVector *roots,
    const uint32_t *ids, uint32_t count) {
    const XrLirModule *input = c->input;
    size_t base = (size_t)XR_LIR_TYPE_COUNT + xr_lir_constructed_type_count(input);
    size_t functions = xr_lir_function_count(input);
    if (base > SIZE_MAX - XR_RBC_ARRAY_FAMILY_COUNT ||
        functions > SIZE_MAX - base - XR_RBC_ARRAY_FAMILY_COUNT)
        return rbc_lir_fail(c,XR_RBC_COMPILE_LIMIT,"value root bitmap overflows");
    uint8_t *seen = xr_calloc(base + XR_RBC_ARRAY_FAMILY_COUNT + functions,1);
    if (!seen) return rbc_lir_fail(c,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own value root bitmap");
    XrLirCallableFacts *facts = NULL;
    XrLirStatus collected = xr_lir_callable_facts_create(input,&facts);
    bool valid = collected == XR_LIR_OK;
    if (!valid) rbc_lir_fail(c,collected == XR_LIR_OUT_OF_MEMORY ? XR_RBC_COMPILE_OUT_OF_MEMORY :
        XR_RBC_COMPILE_INVALID,"cannot receive callable root facts");
    for (uint32_t f = 0; valid && f < count; ++f) {
        XrLirFunction function; XrLirFunctionView view;
        function=(XrLirFunction){input,ids[f]}; xr_lir_function_view(input,function,&view);
        for (size_t index = 0; valid && index <= view.value_count; ++index) {
            XrLirType type = view.result; XrLirValueLayoutKey key;
            if (index) xr_lir_value_type(input,(XrLirValue){input,function.id,(uint32_t)index},&type);
            if (rbc_home_key(input,function,(uint32_t)index,type,&key)) valid = rbc_home_root_append(c,roots,seen,base,key);
        }
        XrLirClosureTarget declaration={0};
        if (valid && xr_lir_closure_target_view(input,function,&declaration)==XR_LIR_OK && declaration.callable)
            valid=rbc_home_root_append(c,roots,seen,base,(XrLirValueLayoutKey){declaration.callable,0,0});
        XrLirCallableFunctionFacts callable={0};
        if (valid && facts && xr_lir_callable_facts_function(facts,function.id,&callable)!=XR_LIR_OK)
            valid=rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"callable roots lost their function facts");
        for (uint32_t v=1;valid && v<=callable.value_count;++v) {
            const XrLirCallableValueFact *fact=&callable.values[v];
            if (fact->operation!=XR_LIR_FUNCTION_REF) continue;
            XrLirClosureTarget target={0};
            if (xr_lir_closure_target_view(input,(XrLirFunction){input,fact->known_target},&target)!=XR_LIR_OK)
                valid=rbc_lir_fail(c,XR_RBC_COMPILE_INVALID,"callable root lost its exact target");
            else {
                XrLirValueLayoutKey key={target.callable,0,0};
                valid=rbc_home_root_append(c,roots,seen,base,key);
                if (valid && fact->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED) {
                    key.environment_function=fact->known_target;
                    valid=rbc_home_root_append(c,roots,seen,base,key);
                }
            }
        }
    }
    for (uint32_t i = 0; valid && state && i < state->slot_count; ++i) {
        const XrLirStateSlot *slot = &state->slots[i];
        XrLirValueLayoutKey key;
        if (slot->payload == XR_LIR_ARRAY) key = (XrLirValueLayoutKey){XR_LIR_ARRAY,slot->array_element,0};
        else if (!rbc_home_key(input,(XrLirFunction){0},0,slot->payload,&key)) {
            valid = rbc_lir_fail(c,XR_RBC_COMPILE_UNSUPPORTED,"State payload has no fixed value plan"); break;
        }
        valid = rbc_home_root_append(c,roots,seen,base,key);
    }
    xr_lir_callable_facts_destroy(facts); xr_free(seen);
    return valid;
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
static void rbc_value_preparation_clear(RbcValuePreparation *preparation) {
    xr_lir_ownership_plan_destroy(preparation->ownership);
    xr_lir_value_layouts_destroy(preparation->layouts);
    xrt_billing_owner_drop(preparation->billing);
    *preparation = (RbcValuePreparation){0};
}
static XrRbcCompileStatus rbc_lir_prepare_values(const XrLirModule *input, XrLirFunction entry,
    const uint32_t *ids, uint32_t count, XrRbcProgram *program,
    RbcValuePreparation *preparation, XrRbcCompileDiagnostic *diagnostic) {
    XrLirFunction state_entry = {0}; XrLirStateLayout state = {0};
    xr_lir_state_entry(input,&state_entry); xr_lir_state_layout_view(input,&state);
    bool receive_state = state_entry.id == entry.id && state.module_count;
    if (!receive_state && !rbc_home_needed(input,ids,count)) return XR_RBC_COMPILE_OK;
    RbcLirCompiler c = {.input=input,.diagnostic=diagnostic}; RbcHomeVector roots = {0};
    if (!rbc_home_roots(&c,receive_state ? &state : NULL,&roots,ids,count)) { xr_free(roots.data); return c.status; }
    XrtManagedStatus memory = xrt_billing_owner_new(NULL,SIZE_MAX,&preparation->billing);
    XrRbcCompileStatus status = memory == XRT_MANAGED_OK ? XR_RBC_COMPILE_OK :
        memory == XRT_MANAGED_OOM ? XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_LIMIT;
    if (status == XR_RBC_COMPILE_OK)
        status = rbc_home_layout_status(xr_lir_value_layouts_create(input,roots.data,roots.count,
            preparation->billing,&preparation->layouts));
    xr_free(roots.data);
    if (status == XR_RBC_COMPILE_OK) {
        const XrLirOwnershipTypeFact *types = NULL; uint32_t fact_count = 0;
        xr_lir_value_layouts_ownership(preparation->layouts,&types,&fact_count);
        XrLirDiagnostic invalid = {0};
        /* Public finish fully verified this exact immutable module. The original
         * paranoid entry check has also finished before any preparation starts. */
        XrLirStatus collected = xr_lir_internal_ownership_collect(&(XrLirOwnershipRequest){input,types,fact_count},
            &preparation->ownership,&invalid);
        if (collected != XR_LIR_OK) {
            status = collected == XR_LIR_OUT_OF_MEMORY ? XR_RBC_COMPILE_OUT_OF_MEMORY : XR_RBC_COMPILE_INVALID;
            diagnostic->function = invalid.function; diagnostic->block = invalid.block;
            diagnostic->instruction = invalid.instruction;
            return rbc_lir_error(diagnostic,status,invalid.message,invalid.source);
        }
        status = rbc_home_layout_status(xr_lir_value_image_create(preparation->layouts,
            preparation->billing,&program->value_image));
    }
    if (status == XR_RBC_COMPILE_OK && receive_state) {
        status = rbc_home_layout_status(xr_lir_state_projection_create(input,preparation->layouts,
            xr_lir_value_image_view(program->value_image),preparation->billing,&program->state_projection));
        if (status == XR_RBC_COMPILE_OK) program->module.state_spec = xr_lir_state_projection_view(program->state_projection);
    }
    return status == XR_RBC_COMPILE_OK ? status :
        rbc_lir_error(diagnostic,status,"cannot prepare fixed physical values",(XrLirSource){0});
}
static XrRbcCompileStatus rbc_lir_value_homes(const XrLirModule *input, const uint32_t *ids,
    XrRbcProgram *program, const RbcValuePreparation *preparation, XrRbcCompileDiagnostic *diagnostic) {
    if (!preparation->layouts) return XR_RBC_COMPILE_OK;
    program->value_homes = xr_calloc(program->module.function_count,sizeof(*program->value_homes));
    if (!program->value_homes) return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,
        "cannot own activation homes",(XrLirSource){0});
    for (size_t f = 0; f < program->module.function_count; ++f) {
        XrLirValueHomesRequest request = {input,{input,ids[f]},preparation->layouts,preparation->ownership};
        XrRbcCompileStatus status = rbc_home_layout_status(xr_lir_value_homes_create(&request,&program->value_homes[f]));
        if (status != XR_RBC_COMPILE_OK) {
            diagnostic->function = ids[f];
            return rbc_lir_error(diagnostic,status,"cannot project activation homes",(XrLirSource){0});
        }
    }
    return XR_RBC_COMPILE_OK;
}
