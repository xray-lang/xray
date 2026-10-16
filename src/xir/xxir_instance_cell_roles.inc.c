/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_cell_roles.inc.c - Immutable checked roles for actual frames
 *
 * KEY CONCEPT:
 *   Synthetic initialization relays retain a real function binding and grant no role.
 */
static XrXirCellParameterRole instance_cell_role(void *context, uint32_t entry, uint32_t parameter) {
    const XrXirProgram *program = context;
    const XrXirProgramPermissions *permissions = program ? program->permissions : NULL;
    if (!permissions || entry >= program->entry_count || permissions->function_count != program->entry_count ||
        parameter >= program->entries[entry].parameter_count)
        return XR_XIR_CELL_ROLE_UNKNOWN;
    XrXirCellProofRole role=XR_XIR_CELL_PROOF_UNKNOWN;
    if (xir_effects_runtime_cell_role(permissions->effects,entry,parameter,&role)!=XR_XIR_OK)
        return XR_XIR_CELL_ROLE_UNKNOWN;
    switch (role) {
    case XR_XIR_CELL_PROOF_OWNED_CAPTURE: return XR_XIR_CELL_ROLE_OWNED_CAPTURE;
    case XR_XIR_CELL_PROOF_SCOPED_REF: return XR_XIR_CELL_ROLE_SCOPED_REF;
    case XR_XIR_CELL_PROOF_LEXICAL_CLEANUP: return XR_XIR_CELL_ROLE_LEXICAL_CLEANUP;
    default: return XR_XIR_CELL_ROLE_UNKNOWN;
    }
}
static void initialization_release(XrXirCallView *view, XrXirCallStatus reason) {
    (void)reason;
    XrXirInstance *instance = instance_current(view->instance);
    XR_CHECK(instance_owner_current(instance), "initializer release requires its current domain");
    if (instance && view->activation == instance->call) xr_xir_value_drop(&instance->pending_function);
}
/* This closes only Cell conditions; all other unknowns remain in the Final mask.
 * Frame preparation independently authenticates and commits each actual loan. */
static XrXirCallStatus instance_entry_cell_root(XrXirCallView *view, const XrXirProgram *program,
    uint32_t entry, const XrXirValue *function, const XrXirValue *arguments, uint32_t count,
    uint32_t *output) {
    const XrXirProgramPermissions *permissions = program->permissions;
    const XrXirCallEntry *target = &program->entries[entry];
    const XrXirFunctionBinding *binding = xr_xir_function_binding(function);
    uint32_t captures = binding ? binding->capture_count : 0;
    if (captures > target->parameter_count || count != target->parameter_count - captures ||
        (count && !arguments) || (binding && (binding->entry != entry || (captures && !binding->captures))))
        return XR_XIR_CALL_BAD_ARGUMENT;
    XirEffectEntryRootView conditional={0};
    if (xir_effects_runtime_entry_root(permissions->effects,entry,&conditional)!=XR_XIR_OK)
        return XR_XIR_CALL_BAD_STATE;
    uint32_t total=conditional.parameter_count;
    XrXirValueAdmission *admission = xr_xir_call_admission(view);
    uint64_t work = (uint64_t)total * 3 + 1;
    if (!admission || !admission->domain) return XR_XIR_CALL_BAD_STATE;
    if (work > admission->work || !xr_xir_domain_work(admission->domain, work)) return XR_XIR_CALL_LIMIT;
    admission->work -= work;
    uint32_t root = conditional.intrinsic_mask;
    if (root & ~(XR_XIR_CALLABLE_ROOT_REQUIRED | XR_XIR_CALLABLE_ROOT_UNRESOLVED)) return XR_XIR_CALL_BAD_STATE;
    for (uint32_t p = 0; p < total; ++p) {
        uint32_t parameter = conditional.parameters[p];
        if (parameter >= target->parameter_count ||
            !xr_xir_type_is_cell(program->types, target->parameters[parameter])) return XR_XIR_CALL_BAD_STATE;
        const XrXirValue *actual = parameter < captures ? &binding->captures[parameter] :
            &arguments[parameter - captures];
        if (!xr_xir_value_argument(actual, program->arena, target->parameters[parameter]) ||
            !xr_xir_cell_in_domain(actual, admission->domain)) return XR_XIR_CALL_BAD_ARGUMENT;
        if (xr_xir_cell_module_storage(actual)) root |= XR_XIR_CALLABLE_ROOT_REQUIRED;
    }
    *output = root;
    return XR_XIR_CALL_READY;
}
