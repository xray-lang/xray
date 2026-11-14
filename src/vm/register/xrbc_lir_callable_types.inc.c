/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_callable_types.inc.c - Receive exact invocation descriptors
 */
static bool rbc_lir_callable_value(const XrLirModule *input, const XrRbcProgram *program,
    XrLirType type, XrLirType element, XrRbcCallableValue *output) {
    XrPodScalar scalar = lir_pod_scalar(type);
    if (scalar < XR_POD_SCALAR_COUNT) {
        *output = (XrRbcCallableValue){XR_RBC_CALLABLE_SCALAR, scalar};
        return true;
    }
    if (type == XR_LIR_STRING || type == XR_LIR_ARRAY || type == XR_LIR_MAP_STRING_I64) {
        uint32_t family = rbc_lir_array_family(element);
        if (type == XR_LIR_ARRAY && family == XR_RBC_ARRAY_FAMILY_COUNT) return false;
        *output = (XrRbcCallableValue){XR_RBC_CALLABLE_REFERENCE,
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
    *output = (XrRbcCallableValue){XR_RBC_CALLABLE_FUNCTION,
        program->physical_callables[type - XR_LIR_TYPE_COUNT]};
    return true;
}

static XrRbcCompileStatus rbc_lir_callable_types(const XrLirModule *input,
    XrRbcProgram *program, XrRbcCompileDiagnostic *diagnostic) {
    uint32_t count = xr_lir_constructed_type_count(input), functions = 0;
    for (uint32_t n = 0; n < count; ++n)
        functions += rbc_lir_callable_type(input, XR_LIR_TYPE_COUNT + n);
    if (!functions) return XR_RBC_COMPILE_OK;
    if (sizeof(uint32_t) > SIZE_MAX / count || sizeof(XrRbcCallableType) > SIZE_MAX / functions)
        return rbc_lir_error(diagnostic, XR_RBC_COMPILE_LIMIT,
            "invocation descriptors exceed address space", (XrLirSource){0});
    program->physical_callables = xr_malloc((size_t)count * sizeof(*program->physical_callables));
    XrRbcCallableType *types = xr_calloc(functions, sizeof(*types));
    if (!program->physical_callables || !types) {
        xr_free(types);
        return rbc_lir_error(diagnostic, XR_RBC_COMPILE_OUT_OF_MEMORY,
            "cannot receive invocation descriptors", (XrLirSource){0});
    }
    program->module.callable_types = types;
    program->module.callable_type_count = functions;
    uint32_t index = 0;
    for (uint32_t n = 0; n < count; ++n)
        program->physical_callables[n] = rbc_lir_callable_type(input, XR_LIR_TYPE_COUNT + n) ? index++ : UINT32_MAX;
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
        if (!rbc_lir_callable_value(input, program, signature.result, signature.result_element, &types[ordinal].result))
            return rbc_lir_error(diagnostic, XR_RBC_COMPILE_UNSUPPORTED,
                "invocation result has no exact bytecode carrier", (XrLirSource){0});
        for (uint32_t p = 0; p < signature.parameter_count; ++p) {
            XrLirCallableParameter parameter = signature.parameters[p];
            parameters[p].mode = (uint32_t)parameter.mode;
            parameters[p].escaping = parameter.escaping;
            if (!rbc_lir_callable_value(input, program, parameter.type, parameter.array_element, &parameters[p].value))
                return rbc_lir_error(diagnostic, XR_RBC_COMPILE_UNSUPPORTED,
                    "invocation parameter has no exact bytecode carrier", (XrLirSource){0});
        }
    }
    return XR_RBC_COMPILE_OK;
}

static XrRbcCompileStatus rbc_lir_callable_parameters(const XrLirModule *input,
    const uint32_t *ids, uint32_t count, XrRbcProgram *program) {
    bool needed = false;
    for (uint32_t f = 0; f < count; ++f) {
        XrLirFunctionView view;
        xr_lir_function_view(input, (XrLirFunction){input, ids[f]}, &view);
        needed |= view.callable_parameter_count != 0;
    }
    if (!needed) return XR_RBC_COMPILE_OK;
    XrRbcFunctionCallableParameters *functions = xr_calloc(count, sizeof(*functions));
    if (!functions) return XR_RBC_COMPILE_OUT_OF_MEMORY;
    program->module.function_callable_parameters = functions;
    program->module.function_callable_parameter_count = count;
    for (uint32_t f = 0; f < count; ++f) {
        XrLirFunctionView view;
        xr_lir_function_view(input, (XrLirFunction){input, ids[f]}, &view);
        if (!view.callable_parameter_count) continue;
        XrRbcFunctionCallableParameter *rows = xr_calloc(view.callable_parameter_count, sizeof(*rows));
        if (!rows) return XR_RBC_COMPILE_OUT_OF_MEMORY;
        functions[f].rows = rows;
        functions[f].count = view.callable_parameter_count;
        for (uint32_t p = 0; p < view.callable_parameter_count; ++p) {
            XrLirFunctionCallableParameter from = view.callable_parameters[p];
            rows[p].parameter = from.parameter;
            rows[p].contract.mode = (uint32_t)from.contract.mode;
            rows[p].contract.escaping = from.contract.escaping;
            if (!rbc_lir_callable_value(input, program, from.contract.type, from.contract.array_element,
                    &rows[p].contract.value)) return XR_RBC_COMPILE_UNSUPPORTED;
        }
    }
    return XR_RBC_COMPILE_OK;
}
