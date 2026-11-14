/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_record_types.inc.c - Exact structural schemas in the receiving pool
 */
typedef struct RecordProjection {
    XrXirNominalTable table;
    XrXirNominalIdentity *identities;
    XrXirNominalFieldIdentity *fields;
    XrXirType *types;
    char (*labels)[32];
} RecordProjection;

static bool record_schema_equal(const XrXirNominalIdentity *identity, const XrtCallableTypeSpec *spec) {
    if (identity->field_count != spec->parameter_count) return false;
    for (uint32_t f = 0; f < identity->field_count; ++f) {
        XrXirLiteral name = identity->fields[f].name;
        XrtCallableParameterSpec field = spec->parameters[f];
        if (name.length != field.name_length || memcmp(name.bytes, field.name, name.length)) return false;
    }
    return true;
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
    storage = NULL;
    allocated = xr_compile_resources_calloc(resources, records, sizeof(*output->labels), &storage);
    output->labels = storage;
    if (allocated != XR_COMPILE_RESOURCE_OK) return callable_resource_status(allocated);
    storage = NULL;
    allocated = xr_compile_resources_calloc(resources, fields, sizeof(*output->fields), &storage);
    output->fields = storage;
    if (allocated != XR_COMPILE_RESOURCE_OK) return callable_resource_status(allocated);
    storage = NULL;
    allocated = xr_compile_resources_calloc(resources, fields, sizeof(*output->types), &storage);
    output->types = storage;
    if (allocated != XR_COMPILE_RESOURCE_OK) return callable_resource_status(allocated);
    size_t next = 0;
    output->table.identities = output->identities;
    for (uint32_t t = 0; t < count; ++t) {
        XrtCallableTypeSpec spec = specs[t];
        if (spec.kind != XRT_CALLABLE_RECORD) continue;
        if (spec.count || spec.flags || spec.throws || spec.element.kind != XRT_CALLABLE_SCALAR ||
            spec.element.type || spec.result.kind != XRT_CALLABLE_SCALAR || spec.result.type)
            return XR_XIR_VALUE_BAD_ARGUMENT;
        XrXirType *types = spec.parameter_count ? output->types + next : NULL;
        XrXirNominalFieldIdentity *names = spec.parameter_count ? output->fields + next : NULL;
        for (uint32_t f = 0; f < spec.parameter_count; ++f) {
            XrtCallableParameterSpec field = spec.parameters[f];
            if (!field.name || !field.name_length || field.mode || field.escaping ||
                !xrt_callable_type_id(field.value, t, &types[f])) return XR_XIR_VALUE_BAD_ARGUMENT;
            names[f] = (XrXirNominalFieldIdentity){{field.name, field.name_length}, XR_XIR_FIELD_MUTABLE};
        }
        uint32_t schema = 0;
        while (schema < output->table.count && !record_schema_equal(&output->identities[schema], &spec)) ++schema;
        if (schema == output->table.count) {
            int length = snprintf(output->labels[schema], sizeof(*output->labels), "$record_%u", schema);
            XR_DCHECK(length > 0 && (size_t)length < sizeof(*output->labels), "record schema labels fit the numeric bound");
            output->identities[schema] = (XrXirNominalIdentity){.module={"$physical",9}, .name={output->labels[schema],(uint32_t)length},
                .exported=1, .arity=spec.parameter_count, .fields=names, .field_count=spec.parameter_count,
                .kind=XR_XIR_NOMINAL_STRUCT, .flags=XR_XIR_NOMINAL_RECORD};
            ++output->table.count;
        }
        nodes[t + XRT_CALLABLE_PREFIX_COUNT] = (XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,
            .nominal={schema, types, spec.parameter_count, types, spec.parameter_count}};
        next += spec.parameter_count;
    }
    return XR_XIR_VALUE_OK;
}

static void record_projection_free(RecordProjection *projection) {
    xr_compile_resources_free(projection->identities);
    xr_compile_resources_free(projection->fields);
    xr_compile_resources_free(projection->types);
    xr_compile_resources_free(projection->labels);
}
