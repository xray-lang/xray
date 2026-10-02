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
static bool nominal_charge(XrXirCompileContext *b, uint64_t bytes, uint64_t work) {
    if (bytes > SIZE_MAX ||!xir_compile_work(b, work)) return false;
      return true;
}
static XrXirStatus nominal_name(XrXirLiteral name, XrXirCompileContext *b) {
    if (!name.bytes || !name.length) return XR_XIR_BAD_STRUCTURE;
    if (!nominal_charge(b, (uint64_t) name.length + 1, name.length)) return XR_XIR_BUDGET;
    return memchr(name.bytes, 0, name.length) ? XR_XIR_BAD_STRUCTURE : XR_XIR_OK;
}
static XrXirStatus nominal_same_name(XrXirLiteral a, XrXirLiteral b,
                                    XrXirCompileContext *budget, bool *same) {
    *same = false;
    uint64_t work = a.length == b.length ? (uint64_t) a.length + 1 : 1;
    if (!nominal_charge(budget, 0, work)) return XR_XIR_BUDGET;
    *same = a.length == b.length && memcmp(a.bytes, b.bytes, a.length) == 0;
    return XR_XIR_OK;
}
static XrXirStatus nominal_variants(uint32_t kind, const XrXirNominalVariant *variants,
    uint32_t count, uint32_t fields, XrXirCompileContext *b) {
    if (kind > XR_XIR_NOMINAL_CLASS || (count != 0) != (variants != NULL) ||
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
static bool nominal_flags_valid(uint32_t kind, uint32_t flags) {
    return kind == XR_XIR_NOMINAL_CLASS ? flags == XR_XIR_NOMINAL_FINAL : flags == 0;
}
static XrXirStatus nominal_field_begin(uint32_t kind, const XrXirNominalVariant *variants,
    uint32_t count, uint32_t field, XrXirCompileContext *b, uint32_t *begin) {
    *begin = 0;
    if (kind == XR_XIR_NOMINAL_STRUCT || kind == XR_XIR_NOMINAL_CLASS) return XR_XIR_OK;
    for (uint32_t i = 0; i < count; ++i) {
        if (!nominal_charge(b, 0, 1)) return XR_XIR_BUDGET;
        if (field >= variants[i].field_begin && field - variants[i].field_begin < variants[i].field_count) {
            *begin = variants[i].field_begin; return XR_XIR_OK;
        }
    }
    return XR_XIR_BAD_STRUCTURE;
}
static void nominal_variants_free(const XrXirNominalVariant *variants, uint32_t count) {
    for (uint32_t i = 0; variants && i < count; ++i) xr_compile_resources_free((void *) variants[i].name.bytes);
    xr_compile_resources_free((void *) variants);
}
static bool nominal_field_type(const XrXirTypes *types, XrXirType type, uint32_t parameters) {
    if (type == XR_XIR_BOOL || xr_xir_type_is_number(type) || type == XR_XIR_STRING ||
        type == XR_XIR_ATOMIC_I64 || type == XR_XIR_ERROR || type == XR_XIR_PANIC_INFO) return true;
    uint32_t id = (uint32_t) type;
    if (id >= XR_XIR_TYPE_PARAMETER_BASE && id < XR_XIR_TYPE_PARAMETER_LIMIT)
        return id - XR_XIR_TYPE_PARAMETER_BASE < parameters;
    const XrXirTypeNode *node = xr_xir_type_node(types, type);
    return node && (node->kind == XR_XIR_TYPE_ARRAY || node->kind == XR_XIR_TYPE_CALLABLE ||
        node->kind == XR_XIR_TYPE_NOMINAL || node->kind == XR_XIR_TYPE_NULLABLE) &&
        node->parameter_span <= parameters;
}
static XrXirStatus nominal_declaration(const XrXirNominalDeclaration *d,
    const XrXirTypes *types, XrXirCompileContext *b) {
    if (d->exported > 1 || (d->parameter_count != 0) != (d->constraints != NULL) ||
        (d->field_count != 0) != (d->fields != NULL)) return XR_XIR_BAD_STRUCTURE;
    if (d->parameter_count > 65536 || d->parameter_count > b->limits.parameters) return XR_XIR_BUDGET;
    uint64_t bytes = (uint64_t) d->parameter_count * sizeof(*d->constraints) +
        (uint64_t) d->field_count * sizeof(*d->fields);
    if (!nominal_charge(b, bytes, (uint64_t) d->parameter_count + d->field_count)) return XR_XIR_BUDGET;
    b->limits.parameters -= d->parameter_count;
    XrXirStatus status = nominal_name(d->module, b);
    if (status == XR_XIR_OK) status = nominal_name(d->name, b);
    if (status != XR_XIR_OK) return status;
    if (!nominal_flags_valid(d->kind,d->flags)) return XR_XIR_BAD_STRUCTURE;
    status = nominal_variants(d->kind, d->variants, d->variant_count, d->field_count, b);
    if (status != XR_XIR_OK) return status;
    for (uint32_t p = 0; p < d->parameter_count; ++p) {
        status = xr_xir_compile_constraint_structure(b, types, d->constraints[p], d->parameter_count);
        if (status != XR_XIR_OK) return status;
    }
    for (uint32_t f = 0; f < d->field_count; ++f) {
        const XrXirNominalField *field = &d->fields[f];
        if (d->kind == XR_XIR_NOMINAL_ENUM && field->flags) return XR_XIR_BAD_STRUCTURE;
        if (d->kind == XR_XIR_NOMINAL_CLASS && !xr_xir_type_span(types,field->type)) {
            status=xr_xir_compile_class_field_verify(b, types, field->type);
            if (status!=XR_XIR_OK) return status;
        }
        uint32_t visibility = field->flags & (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED);
        if ((field->flags & ~(XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED | XR_XIR_FIELD_MUTABLE)) ||
            visibility == (XR_XIR_FIELD_PRIVATE | XR_XIR_FIELD_PROTECTED)) return XR_XIR_BAD_STRUCTURE;
        status = nominal_name(field->name, b);
        if (status != XR_XIR_OK) return status;
        if (!nominal_field_type(types, field->type, d->parameter_count)) return XR_XIR_BAD_TYPE;
        status = xr_xir_compile_type_expression_shape(b, types, field->type, d->parameter_count);
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
    const XrXirTypes *types, XrXirCompileContext *budget) {
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    if (!table) return XR_XIR_OK;
    if (!table->count || (!!table->declarations == !!table->identities)) return XR_XIR_BAD_STRUCTURE;
    if (table->identities) return nominal_identities_verify(table, budget);
    XrXirCompileContext remaining = *budget;
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
XR_FUNC XrXirStatus xr_xir_compile_nominal_structure_verify(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, const XrXirTypes *types) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!budget) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = xr_xir_compile_types_structure_verify(&remaining, types);
    if (status == XR_XIR_OK && (!types || table != types->nominals))
        status = nominal_table_verify(table, types, &remaining);
    if (status == XR_XIR_OK) *budget = remaining;
    return status;
}
XR_FUNC void xr_xir_compile_nominal_free(XrXirNominalTable *table) {
    if (!table) return;
    if (table->identities) { nominal_free_identities(table); return; }
    for (uint32_t i = 0; table->declarations && i < table->count; ++i) {
        const XrXirNominalDeclaration *d = &table->declarations[i];
        xr_compile_resources_free((void *) d->module.bytes); xr_compile_resources_free((void *) d->name.bytes);
        xr_xir_compile_constraint_array_free((XrXirConstraint *)d->constraints, d->parameter_count);
        nominal_variants_free(d->variants, d->variant_count);
        for (uint32_t f = 0; d->fields && f < d->field_count; ++f)
            xr_compile_resources_free((void *) d->fields[f].name.bytes);
        xr_compile_resources_free((void *) d->fields);
    }
    xr_compile_resources_free((void *) table->declarations); xr_compile_resources_free(table);
}
static bool nominal_copy_name(const XrXirCompileContext *compile_context, XrXirLiteral source, XrXirLiteral *output, XrXirStatus *allocation_status) {
    if ((uint64_t)source.length + 1 > SIZE_MAX ||
        !xir_compile_work(compile_context, (uint64_t)source.length + 1)) {
        *allocation_status = XR_XIR_BUDGET; return false;
    }
    char *bytes = xir_compile_alloc(compile_context, (size_t) source.length + 1, allocation_status);
    if (!bytes) return false;
    memcpy(bytes, source.bytes, source.length); bytes[source.length] = 0;
    *output = (XrXirLiteral) {bytes, source.length}; return true;
}
static bool nominal_variants_copy(const XrXirCompileContext *compile_context, const XrXirNominalVariant *source, uint32_t count,
    const XrXirNominalVariant **output, XrXirStatus *allocation_status) {

    if (!count) return true;
    XrXirNominalVariant *copy = xir_compile_calloc(compile_context, count, sizeof(*copy), allocation_status);
    if (!copy) return false;
    *output = copy;
    for (uint32_t i = 0; i < count; ++i) {
        copy[i].field_begin = source[i].field_begin; copy[i].field_count = source[i].field_count;
        if (!nominal_copy_name(compile_context, source[i].name, &copy[i].name, allocation_status)) return false;
    }
    return true;
}

static bool nominal_copy_declaration(const XrXirCompileContext *compile_context, const XrXirNominalDeclaration *source,
                                     XrXirNominalDeclaration *d, XrXirStatus *allocation_status) {
    d->exported = source->exported; d->kind = source->kind;
    d->variant_count = source->variant_count; d->flags = source->flags;
    if (!nominal_variants_copy(compile_context, source->variants, source->variant_count, &d->variants, allocation_status)) return false;
    if (!nominal_copy_name(compile_context, source->module, &d->module, allocation_status) ||
        !nominal_copy_name(compile_context, source->name, &d->name, allocation_status)) return false;
    XrXirConstraint *constraints = NULL;
    if ((*allocation_status = xr_xir_compile_constraint_array_copy_verified(compile_context, source->constraints, source->parameter_count, &constraints)) != XR_XIR_OK)
        return false;
    d->constraints = constraints; d->parameter_count = source->parameter_count;
    if (source->field_count) {
        XrXirNominalField *fields = xir_compile_calloc(compile_context, source->field_count, sizeof(*fields), allocation_status);
        if (!fields) return false;
        d->fields = fields; d->field_count = source->field_count;
        for (uint32_t f = 0; f < source->field_count; ++f) {
            fields[f].type = source->fields[f].type; fields[f].flags = source->fields[f].flags;
            if (!nominal_copy_name(compile_context, source->fields[f].name, &fields[f].name, allocation_status)) return false;
        }
    }
    return true;
}
XR_FUNC XrXirStatus xr_xir_compile_nominal_clone(const XrXirCompileContext *compile_context, const XrXirNominalTable *table, const XrXirTypes *types, XrXirNominalTable **output) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *budget = &compile_state;
    if (!output) return XR_XIR_BAD_STRUCTURE;

    if (!budget) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining = *budget;
    XrXirStatus status = xr_xir_compile_nominal_structure_verify(&remaining, table, types);
    if (status != XR_XIR_OK || !table) return status;
    return nominal_copy_table(compile_context, table, output);
}

static XrXirStatus nominal_copy_table(const XrXirCompileContext *compile_context, const XrXirNominalTable *table,
                                     XrXirNominalTable **output) {
    XrXirStatus allocation_status = XR_XIR_OK;

    if (!table) { *output = NULL; return XR_XIR_OK; }
    if (table->identities) return nominal_copy_identities(compile_context, table, output);
    if (!xir_compile_work(compile_context, table->count)) return XR_XIR_BUDGET;
    XrXirNominalTable *copy = xir_compile_calloc(compile_context, 1, sizeof(*copy), &allocation_status);
    if (!copy) return allocation_status;
    XrXirNominalDeclaration *declarations = xir_compile_calloc(compile_context, table->count, sizeof(*declarations), &allocation_status);
    if (!declarations) { xr_compile_resources_free(copy); return allocation_status; }
    copy->declarations = declarations; copy->count = table->count;
    for (uint32_t i = 0; i < table->count; ++i)
        if (!nominal_copy_declaration(compile_context, &table->declarations[i], &declarations[i], &allocation_status)) {
            xr_xir_compile_nominal_free(copy); return allocation_status;
        }
    *output = copy; return XR_XIR_OK;
}

static XrXirStatus nominal_nodes_verify(const XrXirTypes *types, XrXirCompileContext *budget) {
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
            XrXirStatus status = xr_xir_compile_type_substitution_matches(budget, types, node->nominal.arguments, node->nominal.argument_count, d->fields[f].type, node->nominal.fields[f]);
            if (status != XR_XIR_OK) return status;
        }

    }
    return XR_XIR_OK;
}
