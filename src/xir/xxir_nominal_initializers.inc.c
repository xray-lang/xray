/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_initializers.inc.c - Construction helper identity and signature validation
 *
 * KEY CONCEPT:
 *   Helper bindings are jointly checked with the authentic graph and declaration authority.
 */
/* Definition-owned construction facts. These bindings are not exported names.
 * Common signature/constraint/effect verification still checks each target. */
static XrXirStatus nominal_initializer_signature(const XrXirCompileContext *context,
    const XrXirModule *m, const XrXirNominalDeclaration *owner,
    uint32_t function, const XrXirNominalField *field) {
    const XrXirGeneric empty = {0};
    const XrXirGeneric *generic = m->generics ? &m->generics[function] : &empty;
    if (generic->parameter_count != owner->parameter_count) return XR_XIR_BAD_TYPE;
    uint32_t count = owner->parameter_count;
    for (uint32_t p = 0; p < count; ++p) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (xr_xir_binder_kind(generic,p) != XR_XIR_BINDER_TYPE) return XR_XIR_BAD_TYPE;
        XrXirStatus status = xr_xir_compile_constraint_records_match(context,m->types,
            owner->constraints[p],m->types,generic->constraints[p],count);
        if (status != XR_XIR_OK) return status;
    }
    if (!field) return xr_xir_compile_method_signature_verify(context,m,function);
    uint64_t bytes = (uint64_t)count * sizeof(XrXirType);
    if (bytes > SIZE_MAX || !xir_compile_work(context,count)) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    XrXirType *arguments = count ? xir_compile_alloc(context,(size_t)bytes,&status) : NULL;
    if (count && !arguments) return status;
    for (uint32_t p = 0; p < count; ++p)
        arguments[p] = (XrXirType)(XR_XIR_TYPE_PARAMETER_BASE+p);
    status = xr_xir_compile_type_substitution_matches(context,m->types,arguments,count,
        field->type,m->functions[function].result);
    xr_compile_resources_free(arguments);
    return status;
}
static XrXirStatus nominal_initializer_target(const XrXirCompileContext *context,
    const XrXirModule *m, uint32_t declaration, uint32_t encoded,
    const XrXirNominalField *field) {
    if (!encoded) return XR_XIR_OK;
    const XrXirNominalDeclaration *owner = &m->types->nominals->declarations[declaration];
    if (owner->kind == XR_XIR_NOMINAL_ENUM || owner->native.native_id ||
        xir_effect_evidence_is_instance(m)) return XR_XIR_BAD_STAGE;
    uint32_t function = encoded-1;
    const XrXirDeclarations *d = m->declarations;
    if (function >= m->function_count || !d || !d->functions || !d->modules)
        return XR_XIR_BAD_STRUCTURE;
    const XrXirFunctionIdentity *identity = &d->functions[function];
    const XrXirFunction *body = &m->functions[function];
    if (identity->module >= d->module_count || identity->nominal_owner != declaration+1 ||
        identity->cleanup_owner || identity->test_role || identity->member_access ||
        identity->method_kind != (uint32_t)(field ? XR_XIR_MEMBER_HELPER : XR_XIR_CONSTRUCTOR) ||
        body->parameter_count || body->parameters) return XR_XIR_BAD_STRUCTURE;
    uint32_t visible = owner->exported && (!field ||
        !(field->flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED)));
    if (identity->exported != visible) return XR_XIR_BAD_STRUCTURE;
    const XrXirSourceModule *module = &d->modules[identity->module];
    if (!xir_compile_work(context,owner->module.length)) return XR_XIR_BUDGET;
    if (owner->module.length != module->name_length ||
        memcmp(owner->module.bytes,module->name,owner->module.length)) return XR_XIR_BAD_STRUCTURE;
    if (m->linkage_kind == XR_XIR_PROGRAM && function == d->entry_function) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t i = 0; i < d->module_count; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (function == d->modules[i].initializer) return XR_XIR_BAD_STRUCTURE;
    }
    for (uint32_t i = 0; m->defaults && i < m->defaults->count; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (function == m->defaults->records[i].function) return XR_XIR_BAD_STRUCTURE;
    }
    const XrXirImplementationTable *implementations = d->implementations;
    for (uint32_t i = 0; implementations && i < implementations->count; ++i) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirImplementation *implementation = &implementations->records[i];
        for (uint32_t b = 0; b < implementation->binding_count; ++b) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            if (implementation->bindings[b].function == function) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return nominal_initializer_signature(context,m,owner,function,field);
}
XR_FUNC XrXirStatus xir_construction_verify(
    const XrXirCompileContext *context, const XrXirModule *m, const XrXirConstruction *construction) {
    if (!xir_compile_context_valid(context) || !m) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus shape = xir_construction_shape(context, m->types, construction);
    if (shape != XR_XIR_OK) return shape;
    const XrXirNominalTable *table = m->types ? m->types->nominals : NULL;
    if (!table || m->stage == XR_XIR_LOWERED) return XR_XIR_OK;
    if (table->count && !table->declarations) return XR_XIR_BAD_STRUCTURE;
    bool instance = xir_effect_evidence_is_instance(m);
    for (uint32_t n = 0; n < table->count; ++n) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const XrXirNominalDeclaration *owner = &table->declarations[n];
        const XrXirConstructionRow *row = xr_xir_compile_construction_row(construction, n);
        XrXirStatus status = nominal_initializer_target(context,m,n,row->default_initializer,NULL);
        if (status != XR_XIR_OK) return status;
        /* An ordinary definition has one constructor identity at most. An
         * INSTANCE may contain several concrete origins of the same template. */
        bool constructor = false;
        for (uint32_t f = 0; !instance && row->default_initializer && m->declarations && f < m->function_count; ++f) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            const XrXirFunctionIdentity *id = &m->declarations->functions[f];
            if (id->nominal_owner != n+1 || id->method_kind != XR_XIR_CONSTRUCTOR) continue;
            if (constructor || (row->default_initializer && row->default_initializer != f+1))
                return XR_XIR_BAD_STRUCTURE;
            constructor = true;
        }
        for (uint32_t f = 0; f < owner->field_count; ++f) {
            if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
            const XrXirNominalField *field = &owner->fields[f];
            status = nominal_initializer_target(context,m,n,row->field_initializers[f],field);
            if (status != XR_XIR_OK) return status;
            if (!row->field_initializers[f]) continue;
            for (uint32_t prior = 0; prior < f; ++prior) {
                if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
                if (row->field_initializers[prior] == row->field_initializers[f]) return XR_XIR_BAD_STRUCTURE;
            }
        }
    }
    return XR_XIR_OK;
}
