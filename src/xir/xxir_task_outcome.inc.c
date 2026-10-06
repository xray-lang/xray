/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_outcome.inc.c - Admission before immutable terminal publication
 *
 * KEY CONCEPT:
 *   Erasing an error does not hide its complete underlying enum type.
 */
static XrXirCallStatus task_core_error_admit(XrXirTaskExecutor *executor, const XrXirValue *error) {
    XrXirValue underlying = {0};
    const XrXirTypeArena *arena = executor->config.program->arena;
    const XrXirTypes *types = xr_xir_compile_type_arena_types(arena);
    bool carrier = error->type == XR_XIR_ERROR ? xr_xir_error_borrow(error, &underlying) :
        xr_xir_value_valid(error) && xr_xir_type_is_enum(types, (XrXirType)error->type);
    if (error->type != XR_XIR_ERROR && carrier) underlying = *error;
    if (!carrier || xr_xir_value_arena(error) != arena)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirTypeNode *node = xr_xir_type_node(types, (XrXirType)underlying.type);
    if (!node || node->kind != XR_XIR_TYPE_NOMINAL || node->parameter_span ||
        !types->nominals || !types->nominals->identities || types->nominals->declarations ||
        node->nominal.declaration >= types->nominals->count) return XR_XIR_CALL_BAD_STATE;
    const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
    if (identity->kind != XR_XIR_NOMINAL_ENUM || !identity->variant_count || !identity->variants ||
        node->nominal.field_count != identity->field_count ||
        (!!node->nominal.fields != !!node->nominal.field_count)) return XR_XIR_CALL_BAD_STATE;
    uint32_t end = 0;
    for (uint32_t v = 0; v < identity->variant_count; ++v) {
        if (!xr_xir_domain_work(executor->config.domain, 1)) return XR_XIR_CALL_LIMIT;
        const XrXirNominalVariant *variant = &identity->variants[v];
        if (variant->field_begin != end || variant->field_count > identity->field_count - end)
            return XR_XIR_CALL_BAD_STATE;
        end += variant->field_count;
    }
    if (end != identity->field_count) return XR_XIR_CALL_BAD_STATE;
    for (uint32_t f = 0; f < identity->field_count; ++f) {
        if (!xr_xir_domain_work(executor->config.domain, 1)) return XR_XIR_CALL_LIMIT;
        XrXirType field = node->nominal.fields[f];
        if (field == XR_XIR_BOOL || field == XR_XIR_RUNE || xr_xir_type_is_number(field) || field == XR_XIR_STRING)
            continue;
        /* Closed scalar/string errors are executable. Further immutable
         * payload families need their own complete runtime capability. */
        const XrXirTypeNode *nested = xr_xir_type_node(types, field);
        if (field == XR_XIR_ERROR || field == XR_XIR_PANIC_INFO || !nested ||
            nested->kind == XR_XIR_TYPE_CELL || nested->kind == XR_XIR_TYPE_CALLABLE ||
            xr_xir_type_is_class(types, field)) return XR_XIR_CALL_BAD_STATE;
        return XR_XIR_CALL_UNSUPPORTED;
    }
    XrXirValueAdmission admission = {arena, executor->config.domain, NULL, NULL,
        executor->config.poll_limit, executor->config.call_limit};
    XrXirValueStatus status = xr_xir_value_admit(error, (XrXirType)error->type, &admission);
    return status == XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status == XR_XIR_VALUE_LIMIT ? XR_XIR_CALL_LIMIT :
        status == XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus task_core_outcome_admit(XrXirTaskExecutor *executor, const XrXirCallResult *outcome) {
    if (outcome->status == XR_XIR_CALL_THROWN) return task_core_error_admit(executor, &outcome->value);
    /* Canonical Call admission already checked i64/string returns and the
     * exact scalar FaultDetail plus immutable string panic representation. */
    return XR_XIR_CALL_READY;
}
