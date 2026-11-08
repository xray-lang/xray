/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_primitive.inc.c - Actual owned Input ROOT terminals
 *
 * Primitive facts are derived from current owned Input. Only constructor
 * scratch retains these proposals; neither Input nor Final retains primitive
 * ROOT seed rows.
 */
/* A native terminal is read from the actual owned declaration and complete
 * callable head. It is neither an initializer nor a synthetic storage access.
 * The same reader is used by construction and independent Final replay. */
static XrXirStatus effect_invocation_native_root(const EffectBodyMaterial *body,uint32_t function,bool *output) {
    if (!body || !output || function>=effect_body_count(body)) return XR_XIR_BAD_STRUCTURE;
    const XrXirDeclarations *decls=body->declarations;
    *output=false;
    if (!decls || !decls->functions || decls->functions[function].native_callable!=XR_NATIVE_CALLABLE_SYS_ON_SIGNAL)
        return XR_XIR_OK;
    const EffectBodyFunction *head=&body->functions[function];
    /* Ordinary environments specialize actual callable bounds. Their owned
     * declaration coordinate still names the canonical native header; the
     * specialized actual is not a replacement declaration or a new grant. */
    const EffectBodyMaterial *declaration_body=body;
    uint32_t declaration=function;
    if (body->ordinary_identities) {
        declaration=body->ordinary_identities[function].declaration;
        if (declaration>=body->ordinary_source_count) return XR_XIR_BAD_STRUCTURE;
        if (body->canonical_source) declaration_body=body->canonical_source;
        else {
            while (declaration_body->backbone) declaration_body=declaration_body->backbone;
            if (declaration_body->ordinary_identities) return XR_XIR_BAD_STRUCTURE;
        }
    }
    if (declaration>=effect_body_count(declaration_body) || !declaration_body->declarations ||
        !declaration_body->declarations->functions ||
        declaration_body->declarations->functions[declaration].native_callable!=XR_NATIVE_CALLABLE_SYS_ON_SIGNAL)
        return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *canonical=&declaration_body->functions[declaration];
    const XrXirTypeNode *actual=head->parameter_count==2 && head->parameters?
        xr_xir_type_node(body->domain.types,head->parameters[1]):NULL;
    if (!xir_native_callable_identity(&decls->functions[function]) ||
        !xir_sys_native_scope(decls,function,effect_body_count(body)) ||
        !xir_native_callable_identity(&declaration_body->declarations->functions[declaration]) ||
        !xir_sys_native_scope(declaration_body->declarations,declaration,effect_body_count(declaration_body)) ||
        head->block_count || head->instruction_count || head->run_count || head->operand_count ||
        head->consume_parameter_count || head->parameter_count!=2 || !head->parameters || head->parameters[0]!=XR_XIR_I64 ||
        head->result!=XR_XIR_BOOL || head->error_type!=XR_XIR_NEVER || !actual ||
        actual->kind!=XR_XIR_TYPE_CALLABLE || actual->parameter_count || actual->parameters || actual->parameter_span ||
        actual->result!=XR_XIR_UNIT || actual->element!=XR_XIR_NEVER ||
        !xr_xir_callable_flags_valid(actual->flags) || (actual->flags&XR_XIR_CALLABLE_REST) ||
        (body->generics && (body->generics[function].parameter_count || body->generics[function].argument_count)) ||
        !xir_sys_native_signature(XR_NATIVE_CALLABLE_SYS_ON_SIGNAL,declaration_body->domain.types,
            canonical->parameters,canonical->parameter_count,canonical->result,canonical->error_type)) return XR_XIR_BAD_TYPE;
    *output=true;return XR_XIR_OK;
}

/* A missing buffer unwinds all head/run/type borrows before actual allocation.
 * Every retry derives facts anew; capacity never carries a prior proof result. */
