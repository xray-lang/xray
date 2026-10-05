/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal_identity.inc.c - Template-free nominal identity ownership
 *
 * KEY CONCEPT:
 *   Runtime names and access records have no declaration type expressions.
 */
static XrXirNominalIdentity nominal_identity_header(const XrXirNominalTable *table, uint32_t i) {
    if (table->identities) return table->identities[i];
    const XrXirNominalDeclaration *d = &table->declarations[i];
    return (XrXirNominalIdentity) {d->module, d->name, d->exported, d->parameter_count, NULL, d->field_count, d->kind, d->variants, d->variant_count, d->flags, d->native};
}
static XrXirNominalFieldIdentity nominal_identity_field(const XrXirNominalTable *table, uint32_t i, uint32_t f) {
    if (table->identities) return table->identities[i].fields[f];
    const XrXirNominalField *field = &table->declarations[i].fields[f];
    return (XrXirNominalFieldIdentity) {field->name, field->flags};
}
static XrXirStatus nominal_identities_verify(const XrXirNominalTable *table, XrXirCompileContext *budget) {
    XrXirCompileContext b = *budget;
    if (!nominal_charge(&b, sizeof(*table) + (uint64_t) table->count * sizeof(*table->identities), table->count))
        return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalIdentity *d = &table->identities[i];
        if (d->exported > 1 || (d->field_count != 0) != (d->fields != NULL)) return XR_XIR_BAD_STRUCTURE;
        if (d->arity > 65536 || d->arity > b.limits.parameters) return XR_XIR_BUDGET;
        b.limits.parameters -= d->arity;
        if (!nominal_charge(&b, (uint64_t) d->field_count * sizeof(*d->fields), d->field_count)) return XR_XIR_BUDGET;
        XrXirStatus status = nominal_name(d->module, &b);
        if (status == XR_XIR_OK) status = nominal_name(d->name, &b);
        if (status != XR_XIR_OK) return status;
        if (!nominal_flags_valid(d->kind,d->flags)) return XR_XIR_BAD_STRUCTURE;
        status = nominal_native_verify(&b, d);
        if (status != XR_XIR_OK) return status;
        status = nominal_variants(d->kind, d->variants, d->variant_count, d->field_count, &b);
        if (status != XR_XIR_OK) return status;
        for (uint32_t f = 0; f < d->field_count; ++f) {
            uint32_t flags = d->fields[f].flags;
            if (d->kind == XR_XIR_NOMINAL_ENUM && flags) return XR_XIR_BAD_STRUCTURE;
            if ((flags & ~7u) || (flags & 3u) == 3u) return XR_XIR_BAD_STRUCTURE;
            status = nominal_name(d->fields[f].name, &b);
            if (status != XR_XIR_OK) return status;
            uint32_t begin = 0;
            status = nominal_field_begin(d->kind, d->variants, d->variant_count, f, &b, &begin);
            if (status != XR_XIR_OK) return status;
            for (uint32_t j = begin; j < f; ++j) {
                bool same = false;
                status = nominal_same_name(d->fields[f].name, d->fields[j].name, &b, &same);
                if (status != XR_XIR_OK) return status;
                if (same) return XR_XIR_BAD_STRUCTURE;
            }
        }
        for (uint32_t j = 0; j < i; ++j) {
            bool same = false;
            status = nominal_same_name(d->module, table->identities[j].module, &b, &same);
            if (status != XR_XIR_OK) return status;
            if (!same) continue;
            status = nominal_same_name(d->name, table->identities[j].name, &b, &same);
            if (status != XR_XIR_OK) return status;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    *budget = b; return XR_XIR_OK;
}
static void nominal_free_identities(XrXirNominalTable *table) {
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalIdentity *d = &table->identities[i];
        xr_compile_resources_free((void *) d->module.bytes); xr_compile_resources_free((void *) d->name.bytes);
        for (uint32_t f = 0; d->fields && f < d->field_count; ++f) xr_compile_resources_free((void *) d->fields[f].name.bytes);
        xr_compile_resources_free((void *) d->fields);
        nominal_variants_free(d->variants, d->variant_count);
    }
    xr_compile_resources_free((void *) table->identities); xr_compile_resources_free(table);
}

static XrXirStatus nominal_copy_identities(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, XrXirNominalTable **output) {
    XrXirStatus allocation_status = XR_XIR_OK;

    if (!xir_compile_work(compile_context, table->count)) return XR_XIR_BUDGET;
    XrXirNominalTable *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    XrXirNominalIdentity *identities = xir_compile_calloc(compile_context, table->count, sizeof(*identities), &allocation_status);
    if (!identities) { xr_compile_resources_free(copy); return allocation_status; }
    copy->identities = identities; copy->count = table->count;
    for (uint32_t i = 0; i < table->count; ++i) {
        XrXirNominalIdentity from = nominal_identity_header(table, i), *to = &identities[i];
        to->exported = from.exported; to->arity = from.arity; to->kind = from.kind;
        to->variant_count = from.variant_count; to->flags = from.flags;
        if (!xir_compile_work(compile_context, sizeof(to->native))) { allocation_status = XR_XIR_BUDGET; goto fail; }
        to->native = from.native;
        if (!nominal_variants_copy(compile_context, from.variants, from.variant_count, &to->variants, &allocation_status)) goto fail;
        if (!nominal_copy_name(compile_context, from.module, &to->module, &allocation_status) || !nominal_copy_name(compile_context, from.name, &to->name, &allocation_status)) goto fail;
        XrXirNominalFieldIdentity *fields = from.field_count ? xir_compile_calloc(compile_context, from.field_count, sizeof(*fields), &allocation_status) : NULL;
        if (from.field_count && !fields) goto fail;
        to->fields = fields; to->field_count = from.field_count;
        for (uint32_t f = 0; f < from.field_count; ++f) {
            XrXirNominalFieldIdentity field = nominal_identity_field(table, i, f);
            fields[f].flags = field.flags;
            if (!nominal_copy_name(compile_context, field.name, &fields[f].name, &allocation_status)) goto fail;
        }
    }
    *output = copy; return XR_XIR_OK;
fail:
    nominal_free_identities(copy); return allocation_status;
}
XR_FUNC XrXirStatus xr_xir_compile_nominal_project(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, XrXirNominalTable **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    if (!table || !budget || !table->count || !table->declarations || table->identities) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext b = *budget;
    if (!nominal_charge(&b, sizeof(*table) + (uint64_t) table->count * sizeof(XrXirNominalIdentity), table->count))
        return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        if (!nominal_flags_valid(d->kind,d->flags)) return XR_XIR_BAD_STRUCTURE;
        XrXirNominalIdentity native_identity = nominal_identity_header(table, i);
        XrXirStatus native_status = nominal_native_verify(&b, &native_identity);
        if (native_status != XR_XIR_OK) return native_status;
        XrXirStatus variant_status = nominal_variants(d->kind, d->variants, d->variant_count, d->field_count, &b);
        if (variant_status != XR_XIR_OK) return variant_status;
        if (!nominal_charge(&b, (uint64_t) d->field_count * sizeof(XrXirNominalFieldIdentity), d->field_count))
            return XR_XIR_BUDGET;
        XrXirStatus status = nominal_name(d->module, &b);
        if (status == XR_XIR_OK) status = nominal_name(d->name, &b);
        if (status != XR_XIR_OK) return status;
        for (uint32_t f = 0; f < d->field_count; ++f) {
            status = nominal_name(d->fields[f].name, &b);
            if (status != XR_XIR_OK) return status;
        }
    }
    XrXirStatus status = nominal_copy_identities(compile_context, table, output);
    if (status == XR_XIR_OK) *budget = b;
    return status;
}
