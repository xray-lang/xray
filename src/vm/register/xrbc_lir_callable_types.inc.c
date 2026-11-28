/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_callable_types.inc.c - Receive exact invocation descriptors
 */
static bool rbc_lir_inline_signature_home_type(const XrLirModule *input, XrLirType type) {
    XrLirTypeView shape = {0};
    while (type >= XR_LIR_TYPE_COUNT && xr_lir_type_view(input, type, &shape) == XR_LIR_OK) {
        if (shape.kind == XR_LIR_TYPE_TUPLE ||
            (shape.kind == XR_LIR_TYPE_RECORD && xr_lir_inline_aggregate_type(input, type))) return true;
        if (shape.kind != XR_LIR_TYPE_NULLABLE || shape.element >= type) return false;
        type = shape.element;
        if (type == XR_LIR_STRING || xr_lir_class_payload(input, type, NULL)) return true;
    }
    return false;
}

static bool rbc_lir_payloadless_sum_signature(const XrLirModule *input, XrLirType type) {
    const XrLirRecordField *fields;
    const XrLirSumVariant *variants;
    uint32_t field_count, variant_count;
    return type >= XR_LIR_TYPE_COUNT &&
        xr_lir_sum_variants(input, type, &fields, &field_count, &variants, &variant_count) &&
        variant_count && !field_count;
}

static bool rbc_lir_callable_value(const XrLirModule *input, const XrRbcProgram *program,
    const XrLirValueLayouts *layouts, XrLirType type, XrLirType element, XrRbcCallableValue *output) {
    XrPodScalar scalar = lir_pod_scalar(type);
    if (scalar < XR_POD_SCALAR_COUNT) {
        *output = (XrRbcCallableValue){XR_RBC_CALLABLE_SCALAR, scalar};
        return true;
    }
    uint32_t family = type == XR_LIR_ARRAY ? rbc_lir_array_family(element) : XR_RBC_ARRAY_FAMILY_COUNT;
    bool inline_array = type == XR_LIR_ARRAY && family == XR_RBC_ARRAY_FAMILY_COUNT;
    if (inline_array || rbc_lir_inline_signature_home_type(input, type) ||
        rbc_lir_payloadless_sum_signature(input, type) ||
        (type >= XR_LIR_TYPE_COUNT && xr_lir_nullable_array_element(input, type))) {
        const XrtValuePlan *plan = NULL;
        uint32_t id = 0;
        if ((inline_array && !xr_lir_inline_array_ordinal(input, element)) ||
            xr_lir_value_layouts_lookup(layouts, (XrLirValueLayoutKey){type, element, 0}, &plan, &id) !=
                XR_LIR_VALUE_LAYOUT_OK || !id) return false;
        *output = (XrRbcCallableValue){XR_RBC_CALLABLE_HOME, id};
        return true;
    }
    if (type == XR_LIR_STRING || type == XR_LIR_ARRAY || type == XR_LIR_MAP_STRING_I64 || type == XR_LIR_SET_I64) {
        *output = (XrRbcCallableValue){XR_RBC_CALLABLE_REFERENCE,
            type == XR_LIR_SET_I64 ? XR_RBC_SET_I64 :
            type == XR_LIR_MAP_STRING_I64 ? XR_RBC_MAP_STRING_I64 :
            type == XR_LIR_STRING ? XR_RBC_STRING : xr_rbc_array_kind_at(family)};
        return true;
    }
    if (rbc_lir_aggregate_type(input, type)) {
        uint32_t ordinal = type - XR_LIR_TYPE_COUNT;
        *output = (XrRbcCallableValue){XR_RBC_CALLABLE_AGGREGATE,
            program->physical_aggregates ? program->physical_aggregates[ordinal] : ordinal};
        return true;
    }
    if (!rbc_lir_callable_type(input, type)) return false;
    if (!program->physical_callables) return false;
    uint32_t ordinal = program->physical_callables[type - XR_LIR_TYPE_COUNT];
    if (ordinal == UINT32_MAX) return false;
    *output = (XrRbcCallableValue){XR_RBC_CALLABLE_FUNCTION, ordinal};
    return true;
}

static void rbc_lir_callable_select(const XrLirModule *input, XrLirType type, uint32_t *map) {
    if (rbc_lir_callable_type(input, type)) map[type - XR_LIR_TYPE_COUNT] = 0;
}

static XrRbcCompileStatus rbc_lir_callable_seed(const XrLirModule *input, XrLirType type,
    XrRbcProgram *program, uint32_t count, XrRbcCompileDiagnostic *diagnostic) {
    if (!rbc_lir_callable_type(input, type)) return XR_RBC_COMPILE_OK;
    if (!program->physical_callables) {
        if (sizeof(uint32_t) > SIZE_MAX / count)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_LIMIT,
                "invocation descriptors exceed address space", (XrLirSource){0});
        uint32_t *map = xr_malloc((size_t)count * sizeof(*map));
        if (!map)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_OUT_OF_MEMORY,
                "cannot receive invocation descriptors", (XrLirSource){0});
        for (uint32_t n = 0; n < count; ++n) map[n] = UINT32_MAX;
        program->physical_callables = map;
    }
    program->physical_callables[type - XR_LIR_TYPE_COUNT] = 0;
    return XR_RBC_COMPILE_OK;
}

static XrRbcCompileStatus rbc_lir_callable_types(const XrLirModule *input, const uint32_t *ids, uint32_t reachable,
    XrRbcProgram *program, const XrLirValueLayouts *layouts, XrRbcCompileDiagnostic *diagnostic) {
    uint32_t count = xr_lir_constructed_type_count(input), functions = 0;
    if (!count) return XR_RBC_COMPILE_OK;
    /* The common image includes the exact types used by typed values. Reachable
     * scalar targets can also use a legacy descriptor without a value image. */
    for (size_t n = 0; layouts && n < xr_lir_value_layouts_count(layouts); ++n) {
        XrLirValueLayoutKey key;
        const XrtValuePlan *plan = NULL;
        if (xr_lir_value_layouts_at(layouts, n, &key, &plan) != XR_LIR_VALUE_LAYOUT_OK)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_INVALID,
                "invocation descriptor lost its value image row", (XrLirSource){0});
        if (key.environment_function) continue;
        XrRbcCompileStatus status = rbc_lir_callable_seed(input, key.type, program, count, diagnostic);
        if (status != XR_RBC_COMPILE_OK) return status;
    }
    for (uint32_t n = 0; n < reachable; ++n) {
        XrLirClosureTarget target = {0};
        if (xr_lir_closure_target_view(input, (XrLirFunction){input, ids[n]}, &target) != XR_LIR_OK) continue;
        XrRbcCompileStatus status = rbc_lir_callable_seed(input, target.callable, program, count, diagnostic);
        if (status != XR_RBC_COMPILE_OK) return status;
    }
    uint32_t *map = program->physical_callables;
    if (!map) return XR_RBC_COMPILE_OK;
    /* Callable dependencies precede the receiving signature in the LIR pool. */
    for (uint32_t n = count; n-- > 0;) {
        if (map[n] == UINT32_MAX) continue;
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(input, XR_LIR_TYPE_COUNT + n, &signature) != XR_LIR_OK)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_INVALID,
                "invocation descriptor lost its signature", (XrLirSource){0});
        rbc_lir_callable_select(input, signature.result, map);
        for (uint32_t p = 0; p < signature.parameter_count; ++p)
            rbc_lir_callable_select(input, signature.parameters[p].type, map);
        ++functions;
    }
    assert(functions);
    if (sizeof(XrRbcCallableType) > SIZE_MAX / functions)
        return rbc_lir_error(diagnostic, XR_RBC_COMPILE_LIMIT,
            "invocation descriptors exceed address space", (XrLirSource){0});
    XrRbcCallableType *types = xr_calloc(functions, sizeof(*types));
    if (!types)
        return rbc_lir_error(diagnostic, XR_RBC_COMPILE_OUT_OF_MEMORY,
            "cannot receive invocation descriptors", (XrLirSource){0});
    program->module.callable_types = types;
    program->module.callable_type_count = functions;
    uint32_t index = 0;
    for (uint32_t n = 0; n < count; ++n)
        if (map[n] != UINT32_MAX) map[n] = index++;
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t ordinal = program->physical_callables[n];
        if (ordinal == UINT32_MAX) continue;
        XrLirCallableType signature;
        xr_lir_callable_type_view(input, XR_LIR_TYPE_COUNT + n, &signature);
        if (signature.parameter_count && sizeof(XrRbcCallableParameter) > SIZE_MAX / signature.parameter_count)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_LIMIT,
                "invocation parameters exceed address space", (XrLirSource){0});
        XrRbcCallableParameter *parameters = signature.parameter_count ?
            xr_calloc(signature.parameter_count, sizeof(*parameters)) : NULL;
        if (signature.parameter_count && !parameters)
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_OUT_OF_MEMORY,
                "cannot own invocation parameters", (XrLirSource){0});
        types[ordinal] = (XrRbcCallableType){.parameters=parameters,
            .parameter_count=signature.parameter_count, .flags=signature.flags};
        if (!rbc_lir_callable_value(input, program, layouts, signature.result, signature.result_element, &types[ordinal].result))
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_UNSUPPORTED,
                "invocation result has no exact bytecode carrier", (XrLirSource){0});
        for (uint32_t p = 0; p < signature.parameter_count; ++p) {
            XrLirCallableParameter parameter = signature.parameters[p];
            parameters[p].mode = (uint32_t)parameter.mode;
            parameters[p].escaping = parameter.escaping;
            if (!rbc_lir_callable_value(input, program, layouts, parameter.type, parameter.array_element, &parameters[p].value))
                return rbc_lir_error(diagnostic, XR_RBC_COMPILE_UNSUPPORTED,
                    "invocation parameter has no exact bytecode carrier", (XrLirSource){0});
        }
    }
    return XR_RBC_COMPILE_OK;
}

static XrRbcCompileStatus rbc_lir_callable_parameters(const XrLirModule *input,
    const uint32_t *ids, uint32_t count, XrRbcProgram *program, const XrLirValueLayouts *layouts,
    XrRbcCompileDiagnostic *diagnostic) {
    bool needed = false;
    for (uint32_t f = 0; f < count; ++f) {
        XrLirFunctionView view;
        xr_lir_function_view(input, (XrLirFunction){input, ids[f]}, &view);
        needed |= view.callable_parameter_count != 0;
    }
    if (!needed) return XR_RBC_COMPILE_OK;
    diagnostic->function=ids[0]; diagnostic->block=diagnostic->instruction=0;
    if (count && sizeof(XrRbcFunctionCallableParameters)>SIZE_MAX/count)
        return rbc_lir_error(diagnostic,XR_RBC_COMPILE_LIMIT,
            "callable parameter directory exceeds address space",(XrLirSource){0});
    XrRbcFunctionCallableParameters *functions = xr_calloc(count, sizeof(*functions));
    if (!functions) return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,
        "cannot own callable parameter directory",(XrLirSource){0});
    program->module.function_callable_parameters = functions;
    program->module.function_callable_parameter_count = count;
    for (uint32_t f = 0; f < count; ++f) {
        XrLirFunctionView view;
        xr_lir_function_view(input, (XrLirFunction){input, ids[f]}, &view);
        if (!view.callable_parameter_count) continue;
        diagnostic->function=ids[f];
        if (sizeof(XrRbcFunctionCallableParameter)>SIZE_MAX/view.callable_parameter_count)
            return rbc_lir_error(diagnostic,XR_RBC_COMPILE_LIMIT,
                "callable parameter rows exceed address space",(XrLirSource){0});
        XrRbcFunctionCallableParameter *rows = xr_calloc(view.callable_parameter_count, sizeof(*rows));
        if (!rows) return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,
            "cannot own callable parameter rows",(XrLirSource){0});
        functions[f].rows = rows;
        functions[f].count = view.callable_parameter_count;
        for (uint32_t p = 0; p < view.callable_parameter_count; ++p) {
            XrLirFunctionCallableParameter from = view.callable_parameters[p];
            rows[p].parameter = from.parameter;
            rows[p].contract.mode = (uint32_t)from.contract.mode;
            rows[p].contract.escaping = from.contract.escaping;
            if (!rbc_lir_callable_value(input, program, layouts, from.contract.type, from.contract.array_element,
                    &rows[p].contract.value)) return rbc_lir_error(diagnostic,XR_RBC_COMPILE_UNSUPPORTED,
                        "callable parameter has no exact bytecode carrier",(XrLirSource){0});
        }
    }
    return XR_RBC_COMPILE_OK;
}
static XrRbcCompileStatus rbc_lir_callable_entries(const XrLirModule *input,
    const uint32_t *ids, uint32_t *mapping, XrRbcProgram *program,
    const XrLirValueLayouts *layouts, XrRbcCompileDiagnostic *diagnostic) {
    if (!program->module.callable_type_count) return XR_RBC_COMPILE_OK;
    size_t count=program->module.function_count;
    uint32_t functions=xr_lir_function_count(input);
    for (uint32_t f=0;f<functions;++f) if (mapping[f]==RBC_LIR_ASSERT_ENTRY) ++count;
    if (count>UINT32_MAX)
        return rbc_lir_error(diagnostic,XR_RBC_COMPILE_LIMIT,"callable entry count exceeds indexed storage",(XrLirSource){0});
    if (count>SIZE_MAX/sizeof(*program->callable_entries))
        return rbc_lir_error(diagnostic,XR_RBC_COMPILE_LIMIT,"callable entry map exceeds address space",(XrLirSource){0});
    program->callable_entries=xr_calloc(count,sizeof(*program->callable_entries));
    if (!program->callable_entries)
        return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own callable entry map",(XrLirSource){0});
    program->callable_entry_count=count;
    size_t next=program->module.function_count;
    for (uint32_t f=0;f<functions;++f) if (mapping[f]==RBC_LIR_ASSERT_ENTRY) {
        mapping[f]=(uint32_t)next+1;
        program->callable_entries[next++]=(XrRbcCallableEntry){.physical_function=UINT32_MAX,
            .source_function=f+1,.kind=XR_RBC_CALLABLE_ENTRY_ASSERT_CONDITION};
    }
    for (size_t n=0;n<count;++n) {
        XrRbcCallableEntry *entry=&program->callable_entries[n];
        if (n<program->module.function_count) {
            entry->physical_function=(uint32_t)n; entry->source_function=ids[n];
        }
        XrLirClosureTarget target={0}; const XrtValuePlan *plan=NULL;
        if (layouts && xr_lir_closure_target_view(input,(XrLirFunction){input,entry->source_function},&target)==XR_LIR_OK && target.callable) {
            (void)xr_lir_value_layouts_lookup(layouts,(XrLirValueLayoutKey){target.callable,0,0},&plan,&entry->signature_type);
            entry->callable=program->physical_callables[target.callable-XR_LIR_TYPE_COUNT]+1;
        }
    }
    const XrtValueImage *image=program->value_image ? xr_lir_value_image_view(program->value_image) : NULL;
    if (image && image->type_count) {
        size_t types=image->type_count;
        if (types>SIZE_MAX/sizeof(*program->type_callables))
            return rbc_lir_error(diagnostic,XR_RBC_COMPILE_LIMIT,"callable type map exceeds address space",(XrLirSource){0});
        program->type_callables=xr_calloc(types,sizeof(*program->type_callables));
        if (!program->type_callables)
            return rbc_lir_error(diagnostic,XR_RBC_COMPILE_OUT_OF_MEMORY,"cannot own callable type map",(XrLirSource){0});
        for (uint32_t n=0;n<image->type_count;++n) {
            XrLirValueLayoutKey key; const XrtValuePlan *plan=NULL;
            if (xr_lir_value_layouts_at(layouts,n,&key,&plan)!=XR_LIR_VALUE_LAYOUT_OK)
                return rbc_lir_error(diagnostic,XR_RBC_COMPILE_INVALID,"callable map lost its image row",(XrLirSource){0});
            if (!key.environment_function && rbc_lir_callable_type(input,key.type))
                program->type_callables[n]=program->physical_callables[key.type-XR_LIR_TYPE_COUNT]+1;
        }
    }
    for (uint32_t t=0;image && t<image->type_count;++t) {
        const XrtValueImageType *type=&image->types[t];
        if (type->kind!=XRT_VALUE_ENVIRONMENT) continue;
        uint32_t source=type->entry_ordinal;
        if (!source || source>xr_lir_function_count(input) || !mapping[source-1] || mapping[source-1]>count)
            return rbc_lir_error(diagnostic,XR_RBC_COMPILE_INVALID,"environment has no mapped code entry",(XrLirSource){0});
        XrRbcCallableEntry *entry=&program->callable_entries[mapping[source-1]-1];
        if (entry->environment_type || entry->signature_type!=type->entry_signature)
            return rbc_lir_error(diagnostic,XR_RBC_COMPILE_INVALID,"environment differs from its exact code signature",(XrLirSource){0});
        entry->environment_type=t+1;
    }
    return XR_RBC_COMPILE_OK;
}
