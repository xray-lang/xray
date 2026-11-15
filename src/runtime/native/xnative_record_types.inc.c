/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_record_types.inc.c - Nominal value identities in the receiving pool
 */
typedef struct RecordProjection {
    XrXirNominalTable table;
    XrXirNominalIdentity *identities;
    XrXirNominalFieldIdentity *fields;
    XrXirType *types;
} RecordProjection;

static bool record_schema_equal(const XrXirNominalIdentity *identity, const XrtCallableTypeSpec *spec) {
    if (identity->field_count != spec->count || identity->arity != spec->parameter_count - spec->count ||
        identity->flags != spec->flags || identity->exported != spec->exported) return false;
    for (uint32_t f = 0; f < identity->field_count; ++f) {
        XrXirLiteral name = identity->fields[f].name;
        XrtCallableParameterSpec field = spec->parameters[f];
        if (name.length != field.name_length || memcmp(name.bytes, field.name, name.length) ||
            identity->fields[f].flags != field.field_flags) return false;
    }
    return true;
}
static bool record_qualified_equal(const XrXirNominalIdentity *identity, const XrtCallableTypeSpec *spec) {
    return identity->module.length == spec->module_length && identity->name.length == spec->name_length &&
        !memcmp(identity->module.bytes, spec->module, spec->module_length) &&
        !memcmp(identity->name.bytes, spec->name, spec->name_length);
}

static XrXirValueStatus record_project(const XrtCallableTypeSpec *specs, uint32_t count,
    XrXirTypeNode *nodes, XrCompileResources *resources, RecordProjection *output) {
    size_t fields = 0;
    uint32_t records = 0;
    for (uint32_t t = 0; t < count; ++t) {
        if (specs[t].kind != XRT_CALLABLE_RECORD) continue;
        if (specs[t].parameter_count > SIZE_MAX - fields) return XR_XIR_VALUE_OVERFLOW;
        fields += specs[t].parameter_count;
        ++records;
    }
    if (!records) return XR_XIR_VALUE_OK;
    void *storage = NULL;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(resources, records, sizeof(*output->identities), &storage);
    output->identities = storage;
    if (allocated != XR_COMPILE_RESOURCE_OK) return callable_resource_status(allocated);
    if (fields) {
        storage = NULL;
        allocated = xr_compile_resources_calloc(resources, fields, sizeof(*output->fields), &storage);
        output->fields = storage;
        if (allocated != XR_COMPILE_RESOURCE_OK) return callable_resource_status(allocated);
        storage = NULL;
        allocated = xr_compile_resources_calloc(resources, fields, sizeof(*output->types), &storage);
        output->types = storage;
        if (allocated != XR_COMPILE_RESOURCE_OK) return callable_resource_status(allocated);
    }
    size_t next = 0;
    output->table.identities = output->identities;
    for (uint32_t t = 0; t < count; ++t) {
        XrtCallableTypeSpec spec = specs[t];
        if (spec.kind != XRT_CALLABLE_RECORD) continue;
        if (spec.count > spec.parameter_count || (spec.flags & ~XR_XIR_NOMINAL_MODULE_LITERAL) || spec.throws ||
            !spec.module || !spec.module_length || !spec.name || !spec.name_length || spec.exported > 1 ||
            memchr(spec.module, 0, spec.module_length) || memchr(spec.name, 0, spec.name_length) ||
            spec.element.kind != XRT_CALLABLE_SCALAR || spec.element.type ||
            spec.result.kind != XRT_CALLABLE_SCALAR || spec.result.type)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirType *types = spec.parameter_count ? output->types + next : NULL;
        XrXirNominalFieldIdentity *names = spec.parameter_count ? output->fields + next : NULL;
        for (uint32_t f = 0; f < spec.parameter_count; ++f) {
            XrtCallableParameterSpec field = spec.parameters[f];
            if (field.mode || field.escaping || !xrt_callable_type_id(field.value, t, &types[f]))
                return XR_XIR_VALUE_BAD_ARGUMENT;
            if (f < spec.count) {
                if (!field.name || !field.name_length || memchr(field.name, 0, field.name_length) ||
                    (field.field_flags & ~(XR_XIR_FIELD_PRIVATE|XR_XIR_FIELD_PROTECTED|XR_XIR_FIELD_MUTABLE)) ||
                    (field.field_flags & 3u) == 3u) return XR_XIR_VALUE_BAD_ARGUMENT;
                names[f] = (XrXirNominalFieldIdentity){{field.name, field.name_length}, field.field_flags};
            } else if (field.name || field.name_length || field.field_flags) return XR_XIR_VALUE_BAD_ARGUMENT;
        }
        uint32_t schema = 0;
        while (schema < output->table.count && !record_qualified_equal(&output->identities[schema], &spec)) ++schema;
        if (schema == output->table.count) {
            output->identities[schema] = (XrXirNominalIdentity){.module={spec.module,spec.module_length},
                .name={spec.name,spec.name_length}, .exported=spec.exported,
                .arity=spec.parameter_count - spec.count, .fields=spec.count ? names : NULL, .field_count=spec.count,
                .kind=XR_XIR_NOMINAL_STRUCT, .flags=spec.flags};
            ++output->table.count;
        } else if (!record_schema_equal(&output->identities[schema], &spec)) return XR_XIR_VALUE_BAD_ARGUMENT;
        uint32_t arity = spec.parameter_count - spec.count;
        for (uint32_t prior = 0; prior < t; ++prior) {
            const XrXirTypeNode *node = &nodes[prior + XRT_CALLABLE_PREFIX_COUNT];
            if (node->kind != XR_XIR_TYPE_NOMINAL || node->nominal.declaration != schema) continue;
            if (arity && memcmp(node->nominal.arguments, types + spec.count, (size_t)arity * sizeof(*types))) continue;
            if (spec.count && memcmp(node->nominal.fields, types, (size_t)spec.count * sizeof(*types)))
                return XR_XIR_VALUE_BAD_ARGUMENT;
        }
        nodes[t + XRT_CALLABLE_PREFIX_COUNT] = (XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
            .nominal={schema, arity ? types + spec.count : NULL, arity, spec.count ? types : NULL, spec.count}};
        next += spec.parameter_count;
    }
    return XR_XIR_VALUE_OK;
}

static void record_projection_free(RecordProjection *projection) {
    xr_compile_resources_free(projection->identities);
    xr_compile_resources_free(projection->fields);
    xr_compile_resources_free(projection->types);
}
