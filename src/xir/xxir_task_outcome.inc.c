/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_task_outcome.inc.c - Declared error admission before terminal publication
 *
 * KEY CONCEPT:
 *   The admitted value and immutable worker declaration share one Program.
 */
static XrXirCallStatus task_core_error_admit(XrXirTaskExecutor *executor,
    uint32_t entry, const XrXirValue *error) {
    const XrXirProgram *program=executor->config.program;
    const XrXirProgramPermissions *permissions=program->permissions;
    if (!permissions || entry>=permissions->function_count ||
        permissions->function_count!=program->entry_count ||
        permissions->entries[entry].worker!=XR_XIR_OK || !permissions->input || !permissions->input->artifact)
        return XR_XIR_CALL_BAD_STATE;
    const XrXirModule *module=xr_xir_compile_artifact_module(permissions->input->artifact);
    if (!module || module->function_count!=program->entry_count || !module->functions)
        return XR_XIR_CALL_BAD_STATE;
    XrXirType declared=module->functions[entry].error_type;
    if (xr_xir_function_is_generator(module,entry)) {
        const XrXirTypeNode *iterator=xr_xir_type_node(program->types,module->functions[entry].result);
        if (!iterator || iterator->kind!=XR_XIR_TYPE_ITERATOR) return XR_XIR_CALL_BAD_STATE;
        declared=iterator->result;
    }
    if (declared==XR_XIR_UNIT || declared==XR_XIR_NEVER) return XR_XIR_CALL_BAD_STATE;
    if (error->type!=XR_XIR_ERROR && !xr_xir_type_is_enum(program->types,(XrXirType)error->type))
        return XR_XIR_CALL_BAD_STATE;
    XrXirValueAdmission admission={program->arena,executor->config.domain,NULL,NULL,
        executor->config.poll_limit,executor->config.call_limit};
    XrXirValueStatus admitted=xr_xir_value_admit(error,(XrXirType)error->type,&admission);
    if (admitted!=XR_XIR_VALUE_OK)
        return admitted==XR_XIR_VALUE_LIMIT ? XR_XIR_CALL_LIMIT :
            admitted==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_BAD_STATE;
    /* Admission proved the complete value graph and canonical enum header.
     * Read that header directly instead of repeating an uncharged graph walk. */
    const XirNominalValue *record=(const XirNominalValue *)(uintptr_t)error->payload;
    if (record->object.arena!=program->arena ||
        !xr_xir_type_is_enum(program->types,record->object.type)) return XR_XIR_CALL_BAD_STATE;
    return xr_xir_error_bound_accepts(record->object.type,declared) ?
        XR_XIR_CALL_READY : XR_XIR_CALL_BAD_STATE;
}
static XrXirCallStatus task_core_outcome_admit(XrXirTaskExecutor *executor,
    const XirTask *task, const XrXirCallResult *outcome) {
    if (outcome->status==XR_XIR_CALL_THROWN)
        return task_core_error_admit(executor,task->activation.entry,&outcome->value);
    if (outcome->status==XR_XIR_CALL_RETURNED && !xr_xir_task_scalar_shape((XrXirType)outcome->value.type)) {
        XrXirValueAdmission admission={executor->config.program->arena,executor->config.domain,NULL,NULL,
            executor->config.poll_limit,executor->config.call_limit};
        XrXirValueStatus status=xr_xir_value_admit(&outcome->value,(XrXirType)outcome->value.type,&admission);
        return status==XR_XIR_VALUE_OK ? XR_XIR_CALL_READY : status==XR_XIR_VALUE_LIMIT ? XR_XIR_CALL_LIMIT :
            status==XR_XIR_VALUE_OOM ? XR_XIR_CALL_OOM : XR_XIR_CALL_BAD_STATE;
    }
    return XR_XIR_CALL_READY;
}
