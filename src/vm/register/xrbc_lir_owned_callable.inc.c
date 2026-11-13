/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_lir_owned_callable.inc.c - Owned canonical code and capture directories
 */
static XrRbcCompileStatus rbc_lir_owned_callable(const XrLirModule *input, const uint32_t *ids,
    uint32_t functions, XrRbcProgram *program, XrRbcCompileDiagnostic *diagnostic) {
    uint32_t count = xr_lir_constructed_type_count(input);
    XrtCallableTypeSpec *specs = xr_calloc(count, sizeof(*specs));
    XrtCallableParameterSpec *parameters = NULL;
    size_t parameter_count = 0;
    XrRbcCompileStatus status = XR_RBC_COMPILE_OUT_OF_MEMORY;
    if (count && !specs) goto done;
    for (uint32_t t = 0; t < count; ++t) {
        XrLirCallableType signature;
        if (xr_lir_callable_type_view(input, XR_LIR_TYPE_COUNT + t, &signature) == XR_LIR_OK) {
            if (signature.parameter_count > SIZE_MAX - parameter_count) { status = XR_RBC_COMPILE_LIMIT; goto done; }
            parameter_count += signature.parameter_count;
        }
    }
    if (parameter_count > SIZE_MAX / sizeof(*parameters)) { status = XR_RBC_COMPILE_LIMIT; goto done; }
    parameters = parameter_count ? xr_calloc(parameter_count, sizeof(*parameters)) : NULL;
    if (parameter_count && !parameters) goto done;
    size_t next = 0;
    for (uint32_t t = 0; t < count; ++t) {
        XrLirStatus received = lir_native_callable_spec(input, XR_LIR_TYPE_COUNT + t,
            parameter_count ? parameters + next : NULL,
            parameter_count - next > UINT32_MAX ? UINT32_MAX : (uint32_t)(parameter_count - next), &specs[t]);
        if (received != XR_LIR_OK) { status = XR_RBC_COMPILE_UNSUPPORTED; goto done; }
        next += specs[t].parameter_count;
    }
    XrXirTypeArena *received = xrt_module_callable_catalog(program->module.state_layout);
    XrXirValueStatus built = XR_XIR_VALUE_OK;
    if (received) {
        if (!xr_xir_compile_type_arena_retain(received)) built = XR_XIR_VALUE_REFCOUNT_LIMIT;
        else program->module.closure_types = received;
    } else built = xrt_callable_types_new(specs, count, UINT64_MAX, &program->module.closure_types);
    if (built != XR_XIR_VALUE_OK) {
        status = built == XR_XIR_VALUE_OOM ? XR_RBC_COMPILE_OUT_OF_MEMORY :
            built == XR_XIR_VALUE_LIMIT || built == XR_XIR_VALUE_MEMORY_LIMIT ? XR_RBC_COMPILE_LIMIT : XR_RBC_COMPILE_UNSUPPORTED;
        goto done;
    }
    program->closure_mapping = xr_malloc((size_t)functions * sizeof(*program->closure_mapping));
    XrtCallableEntry *entries = xr_calloc(functions, sizeof(*entries));
    uint32_t *targets = xr_calloc(functions, sizeof(*targets));
    XrXirType *signatures = xr_calloc(program->module.callable_type_count, sizeof(*signatures));
    program->module.closure_entries = entries;
    program->module.closure_targets = targets;
    program->module.closure_signatures = signatures;
    program->module.closure_signature_count = program->module.callable_type_count;
    if (!program->closure_mapping || !entries || !targets || !signatures) goto done;
    for (uint32_t f = 0; f < functions; ++f) program->closure_mapping[f] = UINT32_MAX;
    for (uint32_t t = 0; t < count; ++t)
        if (program->physical_callables[t] != UINT32_MAX)
            signatures[program->physical_callables[t]] = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + 4u + t);
    for (uint32_t f = 0; f < functions; ++f) {
        XrLirFunction function = {input, ids[f]};
        XrLirClosureTarget closure = {0};
        XrLirFunctionView view;
        xr_lir_closure_target_view(input, function, &closure);
        xr_lir_function_view(input, function, &view);
        if (!closure.callable || closure.state_capture) continue;
        bool scalars = true;
        for (uint32_t c = 0; c < closure.capture_count; ++c)
            scalars &= rbc_lir_scalar(view.parameter_types[c]) || rbc_lir_captured_cell(input, view.parameter_types[c]) ||
                rbc_lir_callable_type(input, view.parameter_types[c]);
        if (!scalars) continue;
        uint32_t ordinal = (uint32_t)program->module.closure_entry_count++;
        program->closure_mapping[f] = ordinal;
        targets[ordinal] = f;
        entries[ordinal].type = signatures[program->physical_callables[closure.callable - XR_LIR_TYPE_COUNT]];
        entries[ordinal].capture_count = closure.capture_count;
        XrXirType *captures = closure.capture_count ? xr_calloc(closure.capture_count, sizeof(*captures)) : NULL;
        entries[ordinal].captures = captures;
        if (closure.capture_count && !captures) goto done;
        for (uint32_t c = 0; c < closure.capture_count; ++c) {
            XrtCallableValueSpec value;
            if (!lir_native_callable_value(view.parameter_types[c], 0, &value) ||
                !xrt_callable_type_id(value, count, &captures[c])) { status = XR_RBC_COMPILE_UNSUPPORTED; goto done; }
        }
    }
    status = program->module.closure_entry_count ? XR_RBC_COMPILE_OK : XR_RBC_COMPILE_UNSUPPORTED;
done:
    xr_free(parameters);
    xr_free(specs);
    if (status != XR_RBC_COMPILE_OK)
        rbc_lir_error(diagnostic, status, "cannot receive canonical closure code directory", (XrLirSource){0});
    return status;
}
