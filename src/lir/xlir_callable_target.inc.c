/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_target.inc.c - Authentic code signatures and capture prefixes
 */
XR_FUNC bool xr_lir_internal_callable_parameters_valid(const XrLirModule *module,
    const XrLirType *parameters, uint32_t count, const XrLirFunctionCallableParameter *rows,
    uint32_t row_count, bool implicit_borrows) {
    if ((count && !parameters) || row_count > count || (rows ? !row_count : row_count != 0)) return false;
    uint32_t next = 0;
    for (uint32_t p = 0; p < count; ++p) {
        XrLirCallableType signature;
        if (parameters[p] < XR_LIR_TYPE_COUNT ||
            xr_lir_callable_type_view(module, parameters[p], &signature) != XR_LIR_OK) continue;
        if (!rows) {
            if (!implicit_borrows) return false;
            continue;
        }
        if (next == row_count) return false;
        XrLirFunctionCallableParameter row = rows[next++];
        if (row.parameter != p || row.contract.type != parameters[p] || row.contract.array_element ||
            !xr_param_mode_is_valid(row.contract.mode) || row.contract.mode == XR_PARAM_REF) return false;
    }
    return next == row_count;
}

XR_FUNC bool xr_lir_internal_closure_target_valid(const XrLirModule *m,
    const XrLirFunctionData *function) {
    XrLirClosureTarget target = function->closure;
    if (!target.callable) return !target.capture_count && !target.state_capture;
    XrLirCallableType signature;
    if (function->kind != XR_LIR_FUNCTION_DEFINITION ||
        xr_lir_callable_type_view(m, target.callable, &signature) != XR_LIR_OK ||
        target.capture_count > function->parameter_count ||
        (uint32_t)target.state_capture > function->parameter_count - target.capture_count ||
        signature.parameter_count != function->parameter_count - target.capture_count - target.state_capture ||
        signature.result != function->result || signature.result_element != function->array_result)
        return false;
    if (target.state_capture && function->parameter_types[function->parameter_count - 1] != XR_LIR_STATE)
        return false;
    for (uint32_t p = 0; p < target.capture_count; ++p)
        if (function->parameter_types[p] == XR_LIR_MAP_STRING_I64_CELL ||
            function->parameter_types[p] == XR_LIR_MAP_STRING_I64_REF) return false;
    uint32_t contract = 0;
    for (uint32_t p = 0; p < signature.parameter_count; ++p) {
        uint32_t parameter = target.capture_count + p;
        if (signature.parameters[p].type != function->parameter_types[parameter] ||
            (xr_lir_internal_array_type(signature.parameters[p].type) &&
                signature.parameters[p].array_element != function->values[parameter].array_element)) return false;
        while (contract < function->callable_parameter_count &&
            function->callable_parameters[contract].parameter < parameter) ++contract;
        if (contract < function->callable_parameter_count &&
            function->callable_parameters[contract].parameter == parameter) {
            XrLirCallableParameter actual = function->callable_parameters[contract].contract;
            if (actual.mode != signature.parameters[p].mode || actual.escaping != signature.parameters[p].escaping)
                return false;
        }
    }
    return true;
}

XR_FUNC XrLirStatus xr_lir_set_closure_target(XrLirModule *m, XrLirFunction function,
    XrLirClosureTarget target) {
    XrLirStatus status = lir_writable(m);
    if (status != XR_LIR_OK) return status;
    if (function.module != m || !function.id || function.id > m->function_count || !target.callable)
        return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    XrLirFunctionData pending = m->functions[function.id - 1];
    if (pending.closure.callable) return lir_fail(m, XR_LIR_INVALID_STATE);
    pending.closure = target;
    if (!xr_lir_internal_closure_target_valid(m, &pending)) return lir_fail(m, XR_LIR_INVALID_ARGUMENT);
    m->functions[function.id - 1].closure = target;
    return XR_LIR_OK;
}

XR_FUNC XrLirStatus xr_lir_closure_target_view(const XrLirModule *m, XrLirFunction function,
    XrLirClosureTarget *output) {
    if (!m || m->failure != XR_LIR_OK || !output || function.module != m ||
        !function.id || function.id > m->function_count) return XR_LIR_INVALID_ARGUMENT;
    *output = m->functions[function.id - 1].closure;
    return XR_LIR_OK;
}
