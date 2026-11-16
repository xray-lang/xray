/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * A class field transaction inherits defaults only from its real constructor. */
static XrXirStatus body_call_default_declaration(const XrHirBody *body,
    const XrHirSignatureInput *input, XrCompileRequestControl *control, uint32_t *output) {
    *output = input->function;
    if(input->family==XR_HIR_CALL_INDIRECT&&input->declaration_binding.owner) {
        uint32_t captures=0;
        return xr_hir_body_binding_function_info(body,input->declaration_binding,output,&captures)?
            body_poll(control):XR_XIR_BAD_STRUCTURE;
    }
    if (input->family != XR_HIR_CALL_FUNCTION && input->family != XR_HIR_CALL_CONSTRUCTOR)
        return XR_XIR_OK;
    const XrXirFunctionIdentity *identities = *body->declarations.identities;
    XrXirFunctionIdentity identity = identities[input->function];
    if (identity.method_kind != XR_XIR_CONSTRUCTOR_FIELDS) return XR_XIR_OK;
    if (!identity.nominal_owner || identity.nominal_owner > body->nominal_declarations.count)
        return XR_XIR_BAD_STRUCTURE;
    HirBodyNominalDeclaration owner = ((HirBodyNominalDeclaration *)body->nominal_declarations.items)[identity.nominal_owner - 1];
    uint32_t constructor = owner.constructor;
    if (!owner.received || owner.kind != XR_XIR_NOMINAL_CLASS || !constructor ||
        constructor >= *body->declarations.function_count || constructor == input->function)
        return XR_XIR_BAD_STRUCTURE;
    XrXirFunctionIdentity declaration = identities[constructor];
    if (declaration.method_kind != XR_XIR_CONSTRUCTOR || declaration.nominal_owner != identity.nominal_owner ||
        declaration.module != identity.module || declaration.capture_count || identity.capture_count)
        return XR_XIR_BAD_STRUCTURE;
    XrXirFunction original = (*body->declarations.functions)[constructor];
    XrXirFunction helper = (*body->declarations.functions)[input->function];
    if (original.parameter_count != helper.parameter_count || original.error_type != helper.error_type ||
        original.consume_parameter_count != helper.consume_parameter_count ||
        original.escaping_parameter_count != helper.escaping_parameter_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t p = 0; p < original.parameter_count; ++p) {
        if (!(p % 64)) { XrXirStatus status=body_poll(control); if(status!=XR_XIR_OK)return status; }
        if (original.parameters[p] != helper.parameters[p] ||
            xr_xir_function_parameter_escaping(&original,p) != xr_xir_function_parameter_escaping(&helper,p) ||
            xr_xir_declared_parameter_mode(body->types, original.parameters[p], original.consume_parameters,
                original.consume_parameter_count, p) !=
            xr_xir_declared_parameter_mode(body->types, helper.parameters[p], helper.consume_parameters,
                helper.consume_parameter_count, p)) return XR_XIR_BAD_STRUCTURE;
    }
    *output = constructor;
    return XR_XIR_OK;
}

XR_FUNC bool xr_hir_body_call_default_owner(const XrHirBody *body, XrHirBodySignature signature,
                                          uint32_t *output) {
    if (!body || body->ended || !output || signature.owner != body || signature.index >= body->signatures.count)
        return false;
    HirBodySignature row = ((HirBodySignature *)body->signatures.items)[signature.index];
    if (row.declaration.family != XR_HIR_CALL_FUNCTION && row.declaration.family != XR_HIR_CALL_CONSTRUCTOR && !row.declaration.declaration_binding.owner)
        return false;
    *output = row.default_owner;
    return true;
}
