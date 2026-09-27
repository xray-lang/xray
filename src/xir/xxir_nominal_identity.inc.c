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
    return (XrXirNominalIdentity) {d->module, d->name, d->exported, d->parameter_count, NULL, d->field_count};
}
static XrXirNominalFieldIdentity nominal_identity_field(const XrXirNominalTable *table, uint32_t i, uint32_t f) {
    if (table->identities) return table->identities[i].fields[f];
    const XrXirNominalField *field = &table->declarations[i].fields[f];
    return (XrXirNominalFieldIdentity) {field->name, field->flags};
}
static XrXirStatus nominal_identities_verify(const XrXirNominalTable *table, XrXirBudget *budget) {
    XrXirBudget b = *budget;
    if (!nominal_charge(&b, sizeof(*table) + (uint64_t) table->count * sizeof(*table->identities), table->count))
        return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalIdentity *d = &table->identities[i];
        if (d->exported > 1 || (d->field_count != 0) != (d->fields != NULL)) return XR_XIR_BAD_STRUCTURE;
        if (d->arity > 65536 || d->arity > b.parameters) return XR_XIR_BUDGET;
        b.parameters -= d->arity;
        if (!nominal_charge(&b, (uint64_t) d->field_count * sizeof(*d->fields), d->field_count)) return XR_XIR_BUDGET;
        XrXirStatus status = nominal_name(d->module, &b);
        if (status == XR_XIR_OK) status = nominal_name(d->name, &b);
        if (status != XR_XIR_OK) return status;
        for (uint32_t f = 0; f < d->field_count; ++f) {
            uint32_t flags = d->fields[f].flags;
            if ((flags & ~7u) || (flags & 3u) == 3u) return XR_XIR_BAD_STRUCTURE;
            status = nominal_name(d->fields[f].name, &b);
            if (status != XR_XIR_OK) return status;
            for (uint32_t j = 0; j < f; ++j) {
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
        xr_free((void *) d->module.bytes); xr_free((void *) d->name.bytes);
        for (uint32_t f = 0; d->fields && f < d->field_count; ++f) xr_free((void *) d->fields[f].name.bytes);
        xr_free((void *) d->fields);
    }
    xr_free((void *) table->identities); xr_free(table);
}
static bool nominal_identity_copy_work(const XrXirNominalTable *table, XrXirBudget *budget) {
    for (uint32_t i = 0; i < table->count; ++i) {
        XrXirNominalIdentity d = nominal_identity_header(table, i);
        if (!nominal_charge(budget, 0, (uint64_t) d.module.length + d.name.length + d.field_count + 1)) return false;
        for (uint32_t f = 0; f < d.field_count; ++f)
            if (!nominal_charge(budget, 0, (uint64_t) nominal_identity_field(table, i, f).name.length + 1)) return false;
    }
    return true;
}
static XrXirStatus nominal_copy_identities(const XrXirNominalTable *table, XrXirNominalTable **output) {
    *output = NULL;
    XrXirNominalTable *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    XrXirNominalIdentity *identities = xr_calloc(table->count, sizeof(*identities));
    if (!identities) { xr_free(copy); return XR_XIR_OUT_OF_MEMORY; }
    copy->identities = identities; copy->count = table->count;
    for (uint32_t i = 0; i < table->count; ++i) {
        XrXirNominalIdentity from = nominal_identity_header(table, i), *to = &identities[i];
        to->exported = from.exported; to->arity = from.arity;
        if (!nominal_copy_name(from.module, &to->module) || !nominal_copy_name(from.name, &to->name)) goto fail;
        XrXirNominalFieldIdentity *fields = from.field_count ? xr_calloc(from.field_count, sizeof(*fields)) : NULL;
        if (from.field_count && !fields) goto fail;
        to->fields = fields; to->field_count = from.field_count;
        for (uint32_t f = 0; f < from.field_count; ++f) {
            XrXirNominalFieldIdentity field = nominal_identity_field(table, i, f);
            fields[f].flags = field.flags;
            if (!nominal_copy_name(field.name, &fields[f].name)) goto fail;
        }
    }
    *output = copy; return XR_XIR_OK;
fail:
    nominal_free_identities(copy); return XR_XIR_OUT_OF_MEMORY;
}
XR_FUNC XrXirStatus xr_xir_nominal_project(const XrXirNominalTable *table,
    XrXirBudget *budget, XrXirNominalTable **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!table || !budget || !table->count || !table->declarations || table->identities) return XR_XIR_BAD_STRUCTURE;
    XrXirBudget b = *budget;
    if (!nominal_charge(&b, sizeof(*table) + (uint64_t) table->count * sizeof(XrXirNominalIdentity), table->count))
        return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
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
    XrXirStatus status = nominal_copy_identities(table, output);
    if (status == XR_XIR_OK) *budget = b;
    return status;
}
