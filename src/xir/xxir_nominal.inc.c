/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_nominal.inc.c - Declaration scope validation and owned metadata cloning
 *
 * KEY CONCEPT:
 *   Every failed construction releases its initialized ownership prefix.
 */
static bool nominal_charge(XrXirBudget *b, uint64_t bytes, uint64_t work) {
    if (bytes > SIZE_MAX || bytes > b->metadata_bytes || work > b->work) return false;
    b->metadata_bytes -= bytes; b->work -= work; return true;
}
static XrXirStatus nominal_name(XrXirLiteral name, XrXirBudget *b) {
    if (!name.bytes || !name.length) return XR_XIR_BAD_STRUCTURE;
    if (!nominal_charge(b, (uint64_t) name.length + 1, name.length)) return XR_XIR_BUDGET;
    return memchr(name.bytes, 0, name.length) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus nominal_same_name(XrXirLiteral a, XrXirLiteral b,
                                    XrXirBudget *budget, bool *same) {
    *same = false;
    uint64_t work = a.length == b.length ? (uint64_t) a.length + 1 : 1;
    if (!nominal_charge(budget, 0, work)) return XR_XIR_BUDGET;
    *same = a.length == b.length && memcmp(a.bytes, b.bytes, a.length) == 0;
    return XR_XIR_OK;
}
static XrXirStatus nominal_variants(uint32_t kind, const XrXirNominalVariant *variants,
    uint32_t count, uint32_t fields, XrXirBudget *b) {
    if (kind > XR_XIR_NOMINAL_ENUM || (count != 0) != (variants != NULL) ||
        (kind == XR_XIR_NOMINAL_ENUM) != (count != 0)) return XR_XIR_BAD_STRUCTURE;
    if (!nominal_charge(b, (uint64_t) count * sizeof(*variants), count)) return XR_XIR_BUDGET;
    uint32_t end = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const XrXirNominalVariant *v = &variants[i];
        if (v->field_begin != end || v->field_count > fields - end) return XR_XIR_BAD_STRUCTURE;
        end += v->field_count;
        XrXirStatus status = nominal_name(v->name, b);
        if (status != XR_XIR_OK) return status;
        for (uint32_t j = 0; j < i; ++j) {
            bool same = false; status = nominal_same_name(v->name, variants[j].name, b, &same);
            if (status != XR_XIR_OK) return status;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return kind == XR_XIR_NOMINAL_ENUM && end != fields ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus nominal_field_begin(uint32_t kind, const XrXirNominalVariant *variants,
    uint32_t count, uint32_t field, XrXirBudget *b, uint32_t *begin) {
    *begin = 0;
    if (kind == XR_XIR_NOMINAL_STRUCT) return XR_XIR_OK;
    for (uint32_t i = 0; i < count; ++i) {
        if (!nominal_charge(b, 0, 1)) return XR_XIR_BUDGET;
        if (field >= variants[i].field_begin && field - variants[i].field_begin < variants[i].field_count) {
            *begin = variants[i].field_begin; return XR_XIR_OK;
        }
    }
    return XR_XIR_BAD_STRUCTURE;
}
static void nominal_variants_free(const XrXirNominalVariant *variants, uint32_t count) {
    for (uint32_t i = 0; variants && i < count; ++i) xr_free((void *) variants[i].name.bytes);
    xr_free((void *) variants);
}
static bool nominal_field_type(const XrXirTypes *types, XrXirType type, uint32_t parameters) {
    if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
        type == XR_XIR_ATOMIC_I64 || type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO) return true;
    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE < parameters;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CALLABLE ||
        node->kind == XR_XIR_TYPE_NOMINAL) &&
        node->parameter_span <= parameters;
}
static XrXirStatus nominal_declaration(const XrXirNominalDeclaration *d,
    const XrXirTypes *types, XrXirBudget *b) {
    if (d->exported > 1 || (d->parameter_count != 0) != (d->constraints != NULL) ||
        (d->field_count != 0) != (d->fields != NULL)) return XR_XIR_BAD_STRUCTURE;
    if (d->parameter_count > 65536 || d->parameter_count > b->parameters) return XR_XIR_BUDGET;
    uint64_t bytes = (uint64_t) d->parameter_count * sizeof(*d->constraints) +
        (uint64_t) d->field_count * sizeof(*d->fields);
    if (!nominal_charge(b, bytes, (uint64_t) d->parameter_count + d->field_count)) return XR_XIR_BUDGET;
    b->parameters -= d->parameter_count;
    XrXirStatus status = nominal_name(d->module, b);
    if (status == XR_XIR_OK) status = nominal_name(d->name, b);
    if (status != XR_XIR_OK) return status;
    status = nominal_variants(d->kind, d->variants, d->variant_count, d->field_count, b);
    if (status != XR_XIR_OK) return status;
    for (uint32_t p = 0; p < d->parameter_count; ++p)
        if (d->constraints[p].markers & ~XR_XIR_CONSTRAINT_MASK) return XR_XIR_BAD_TYPE;
    for (uint32_t f = 0; f < d->field_count; ++f) {
        const XrXirNominalField *field = &d->fields[f];
        if (d->kind == XR_XIR_NOMINAL_ENUM && field->flags) return XR_XIR_BAD_STRUCTURE;
        uint32_t visibility = field->flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED);
        if ((field->flags & ~(XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED | XR_XIR_FIELD_MUTABLE)) ||
            visibility == (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED)) return XR_XIR_BAD_STRUCTURE;
        status = nominal_name(field->name, b);
        if (status != XR_XIR_OK) return status;
        if (!nominal_field_type(types, field->type, d->parameter_count)) return XR_XIR_BAD_TYPE;
        status = xr_xir_type_context_verify(types, field->type, d->constraints, d->parameter_count, b);
        if (status != XR_XIR_OK) return status;
        uint32_t begin = 0;
        status = nominal_field_begin(d->kind, d->variants, d->variant_count, f, b, &begin);
        if (status != XR_XIR_OK) return status;
        for (uint32_t j = begin; j < f; ++j) {
            bool same = false;
            status = nominal_same_name(field->name, d->fields[j].name, b, &same);
            if (status != XR_XIR_OK) return status;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    return XR_XIR_OK;
}
static XrXirStatus nominal_table_verify(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirBudget *budget) {
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    if (!table) return XR_XIR_OK;
    if (!table->count || (!!table->declarations == !!table->identities)) return XR_XIR_BAD_STRUCTURE;
    if (table->identities) return nominal_identities_verify(table, budget);
    XrXirBudget remaining = *budget;
    XrXirStatus status = XR_XIR_OK;
    if (!nominal_charge(&remaining, sizeof(*table) +
        (uint64_t) table->count * sizeof(*table->declarations), table->count)) return XR_XIR_BUDGET;
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        status = nominal_declaration(d, types, &remaining);
        if (status != XR_XIR_OK) return status;
        for (uint32_t j = 0; j < i; ++j) {
            bool same = false;
            status = nominal_same_name(d->module, table->declarations[j].module, &remaining, &same);
            if (status != XR_XIR_OK) return status;
            if (!same) continue;
            status = nominal_same_name(d->name, table->declarations[j].name, &remaining, &same);
            if (status != XR_XIR_OK) return status;
            if (same) return XR_XIR_BAD_STRUCTURE;
        }
    }
    *budget = remaining; return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_nominal_verify(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirBudget *budget) {
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    XrXirBudget remaining = *budget;
    XrXirStatus status = xr_xir_types_verify(types, &remaining);
    if (status == XR_XIR_OK && (!types || table != types->nominals))
        status = nominal_table_verify(table, types, &remaining);
    if (status == XR_XIR_OK) *budget = remaining;
    return status;
}
XR_FUNC void xr_xir_nominal_free(XrXirNominalTable *table) {
    if (!table) return;
    if (table->identities) { nominal_free_identities(table); return; }
    for (uint32_t i = 0; table->declarations && i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        xr_free((void *) d->module.bytes); xr_free((void *) d->name.bytes);
        xr_free((void *) d->constraints);
        nominal_variants_free(d->variants, d->variant_count);
        for (uint32_t f = 0; d->fields && f < d->field_count; ++f)
            xr_free((void *) d->fields[f].name.bytes);
        xr_free((void *) d->fields);
    }
    xr_free((void *) table->declarations); xr_free(table);
}
static bool nominal_copy_name(XrXirLiteral source, XrXirLiteral *output) {
    char *bytes = xr_malloc((size_t) source.length + 1);
    if (!bytes) return false;
    memcpy(bytes, source.bytes, source.length); bytes[source.length] = 0;
    *output = (XrXirLiteral) {bytes, source.length}; return true;
}
static bool nominal_variants_copy(const XrXirNominalVariant *source, uint32_t count,
    const XrXirNominalVariant **output) {
    *output = NULL;
    if (!count) return true;
    XrXirNominalVariant *copy = xr_calloc(count, sizeof(*copy));
    if (!copy) return false;
    *output = copy;
    for (uint32_t i = 0; i < count; ++i) {
        copy[i].field_begin = source[i].field_begin; copy[i].field_count = source[i].field_count;
        if (!nominal_copy_name(source[i].name, &copy[i].name)) return false;
    }
    return true;
}
static bool nominal_variants_copy_work(const XrXirNominalVariant *variants, uint32_t count, XrXirBudget *b) {
    if (!nominal_charge(b, 0, count)) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!nominal_charge(b, 0, (uint64_t) variants[i].name.length + 1)) return false;
    return true;
}
static bool nominal_copy_declaration(const XrXirNominalDeclaration *source,
                                     XrXirNominalDeclaration *d) {
    d->exported = source->exported; d->kind = source->kind;
    d->variant_count = source->variant_count;
    if (!nominal_variants_copy(source->variants, source->variant_count, &d->variants)) return false;
    if (!nominal_copy_name(source->module, &d->module) ||
        !nominal_copy_name(source->name, &d->name)) return false;
    if (source->parameter_count) {
        size_t bytes = (size_t) source->parameter_count * sizeof(*source->constraints);
        XrXirConstraint *constraints = xr_malloc(bytes);
        if (!constraints) return false;
        memcpy(constraints, source->constraints, bytes);
        d->constraints = constraints; d->parameter_count = source->parameter_count;
    }
    if (source->field_count) {
        XrXirNominalField *fields = xr_calloc(source->field_count, sizeof(*fields));
        if (!fields) return false;
        d->fields = fields; d->field_count = source->field_count;
        for (uint32_t f = 0; f < source->field_count; ++f) {
            fields[f].type = source->fields[f].type; fields[f].flags = source->fields[f].flags;
            if (!nominal_copy_name(source->fields[f].name, &fields[f].name)) return false;
        }
    }
    return true;
}
XR_FUNC XrXirStatus xr_xir_nominal_clone(const XrXirNominalTable *table,
    const XrXirTypes *types, XrXirBudget *budget, XrXirNominalTable **output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    *output = NULL;
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    XrXirBudget remaining = *budget;
    XrXirStatus status = xr_xir_nominal_verify(table, types, &remaining);
    if (status != XR_XIR_OK || !table) return status;
    if (table->identities) {
        if (!nominal_identity_copy_work(table, &remaining)) return XR_XIR_BUDGET;
        status = nominal_copy_identities(table, output);
        if (status == XR_XIR_OK) *budget = remaining;
        return status;
    }
    for (uint32_t i = 0; i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        if (!nominal_variants_copy_work(d->variants, d->variant_count, &remaining)) return XR_XIR_BUDGET;
        uint64_t work = 1 + (uint64_t) d->module.length + d->name.length +
            d->parameter_count + d->field_count;
        if (!nominal_charge(&remaining, 0, work)) return XR_XIR_BUDGET;
        for (uint32_t f = 0; f < d->field_count; ++f)
            if (!nominal_charge(&remaining, 0, (uint64_t) d->fields[f].name.length + 1))
                return XR_XIR_BUDGET;
    }
    status = nominal_copy_table(table, output);
    if (status == XR_XIR_OK) *budget = remaining;
    return status;
}

static XrXirStatus nominal_copy_table(const XrXirNominalTable *table,
                                     XrXirNominalTable **output) {
    *output = NULL;
    if (!table) return XR_XIR_OK;
    if (table->identities) return nominal_copy_identities(table, output);
    XrXirNominalTable *copy = xr_calloc(1, sizeof(*copy));
    if (!copy) return XR_XIR_OUT_OF_MEMORY;
    XrXirNominalDeclaration *declarations = xr_calloc(table->count, sizeof(*declarations));
    if (!declarations) { xr_free(copy); return XR_XIR_OUT_OF_MEMORY; }
    copy->declarations = declarations; copy->count = table->count;
    for (uint32_t i = 0; i < table->count; ++i)
        if (!nominal_copy_declaration(&table->declarations[i], &declarations[i])) {
            xr_xir_nominal_free(copy); return XR_XIR_OUT_OF_MEMORY;
        }
    *output = copy; return XR_XIR_OK;
}

static XrXirStatus nominal_nodes_verify(const XrXirTypes *types, XrXirBudget *budget) {
    if (!types->nominals) return XR_XIR_OK;
    for (uint32_t i = 0; i < types->count; ++i) {
        if (!nominal_charge(budget, 0, 1)) return XR_XIR_BUDGET;
        const XrXirTypeNode *node = &types->nodes[i];
        if (node->kind != XR_XIR_TYPE_NOMINAL) continue;
        if (!types->nominals || node->nominal.declaration >= types->nominals->count) return XR_XIR_BAD_TYPE;
        if (types->nominals->identities) {
            const XrXirNominalIdentity *identity = &types->nominals->identities[node->nominal.declaration];
            if (node->nominal.argument_count != identity->arity || node->nominal.field_count != identity->field_count)
                return XR_XIR_BAD_TYPE;
            for (uint32_t f = 0; f < node->nominal.field_count; ++f) {
                if (!nominal_charge(budget, 0, 1)) return XR_XIR_BUDGET;
                if (!nominal_field_type(types, node->nominal.fields[f], 0)) return XR_XIR_BAD_TYPE;
            }
            continue;
        }
        const XrXirNominalDeclaration *d = &types->nominals->declarations[node->nominal.declaration];
        if (node->nominal.argument_count != d->parameter_count) return XR_XIR_BAD_TYPE;
        if (node->nominal.field_count && node->nominal.field_count != d->field_count) return XR_XIR_BAD_TYPE;
        for (uint32_t f = 0; f < node->nominal.field_count; ++f) {
            if (!nominal_field_type(types, node->nominal.fields[f], node->parameter_span)) return XR_XIR_BAD_TYPE;
            XrXirStatus status = xr_xir_type_substitution_matches(types, node->nominal.arguments,
                node->nominal.argument_count, d->fields[f].type, node->nominal.fields[f], budget);
            if (status != XR_XIR_OK) return status;
        }
        for (uint32_t a = 0; a < d->parameter_count; ++a) {
            if (!nominal_charge(budget, 0, 1)) return XR_XIR_BUDGET;
            if (!xr_xir_type_span(types, node->nominal.arguments[a]) &&
                d->constraints[a].markers) {
                XrXirStatus status = xr_xir_type_markers(types, node->nominal.arguments[a], d->constraints[a].markers, NULL, 0, &budget->work);
                if (status != XR_XIR_OK) return status;
            }
        }
    }
    return XR_XIR_OK;
}
