/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_import.inc.c - Iterative receiving of constructed dependencies
 *
 * KEY CONCEPT:
 *   Dependencies precede their user. A reverse marking pass and a forward
 *   receiving pass import only the requested shape without recursive C calls.
 */
static void lir_import_dependency(uint8_t *needed, XrLirType dependency) {
    if (dependency >= XR_LIR_TYPE_COUNT) needed[dependency - XR_LIR_TYPE_COUNT] = 1;
}

static XrLirType lir_imported_dependency(const XrLirType *mapping, XrLirType type) {
    return type < XR_LIR_TYPE_COUNT ? type : mapping[type - XR_LIR_TYPE_COUNT];
}

static XrLirStatus lir_import_dependencies(XrLirModule *target, const XrLirModule *source,
    XrLirType type, XrLirType *output) {
    if (target == source) { *output = type; return XR_LIR_OK; }
    uint32_t count = type - XR_LIR_TYPE_COUNT + 1;
    uint8_t *needed = lir_allocate(target, count, sizeof(*needed));
    XrLirType *mapping = lir_allocate(target, count, sizeof(*mapping));
    XrLirCallableParameter *parameters = NULL;
    XrLirRecordField *fields = NULL;
    XrLirRecordArgument *arguments = NULL;
    XrLirSumVariant *variants = NULL;
    XrLirStatus status = target->failure;
    if (!needed || !mapping) goto done;
    memset(needed, 0, count * sizeof(*needed));
    memset(mapping, 0, count * sizeof(*mapping));
    needed[count - 1] = 1;
    uint32_t largest = 0, largest_record = 0, largest_arguments = 0, largest_variants = 0, largest_sum = 0;
    for (uint32_t n = count; n-- > 0;) {
        if (!needed[n]) continue;
        const XrLirTypeData *data = &source->types[n];
        lir_import_dependency(needed, data->shape.element);
        if (lir_nullable_array_shape(data->shape)) lir_import_dependency(needed, data->cell_element);
        lir_import_dependency(needed, data->class_parent);
        if (data->shape.kind == XR_LIR_TYPE_RECORD || data->shape.kind == XR_LIR_TYPE_TUPLE) {
            if (data->shape.count > largest_record) largest_record = data->shape.count;
            for (uint32_t f = 0; f < data->shape.count; ++f) lir_import_dependency(needed, data->fields[f].type);
        }
        if (data->record.argument_count > largest_arguments) largest_arguments = data->record.argument_count;
        for (uint32_t a = 0; a < data->record.argument_count; ++a)
            lir_import_dependency(needed, data->record.arguments[a].type);
        if (data->shape.kind == XR_LIR_TYPE_SUM) {
            if (data->variant_count > largest_variants) largest_variants = data->variant_count;
            uint32_t total = 0;
            for (uint32_t v = 0; v < data->variant_count; ++v) {
                for (uint32_t f = 0; f < data->variants[v].field_count; ++f)
                    lir_import_dependency(needed, data->fields[data->variants[v].field_begin + f].type);
                total += data->variants[v].field_count;
            }
            if (total > largest_sum) largest_sum = total;
        }
        if (data->shape.kind != XR_LIR_TYPE_CALLABLE) continue;
        if (data->callable.parameter_count > largest) largest = data->callable.parameter_count;
        for (uint32_t p = 0; p < data->callable.parameter_count; ++p)
            lir_import_dependency(needed, data->callable.parameters[p].type);
    }
    parameters = largest ? lir_allocate(target, largest, sizeof(*parameters)) : NULL;
    if (largest && !parameters) { status = target->failure; goto done; }
    uint32_t largest_fields = largest_record > largest_sum ? largest_record : largest_sum;
    fields = largest_fields ? lir_allocate(target, largest_fields, sizeof(*fields)) : NULL;
    if (largest_fields && !fields) { status = target->failure; goto done; }
    arguments = largest_arguments ? lir_allocate(target, largest_arguments, sizeof(*arguments)) : NULL;
    if (largest_arguments && !arguments) { status = target->failure; goto done; }
    variants = largest_variants ? lir_allocate(target, largest_variants, sizeof(*variants)) : NULL;
    if (largest_variants && !variants) { status = target->failure; goto done; }
    for (uint32_t n = 0; n < count && status == XR_LIR_OK; ++n) {
        if (!needed[n]) continue;
        const XrLirTypeData *data = &source->types[n];
        XrLirTypeView shape = data->shape;
        shape.element = lir_imported_dependency(mapping, shape.element);
        XrLirCallableType callable = data->callable;
        XrLirRecordIdentity record = data->record;
        uint32_t sum_fields = 0;
        if (shape.kind == XR_LIR_TYPE_SUM) {
            for (uint32_t v = 0; v < data->variant_count; ++v) {
                variants[v] = data->variants[v];
                for (uint32_t f = 0; f < variants[v].field_count; ++f) {
                    fields[sum_fields] = data->fields[variants[v].field_begin + f];
                    fields[sum_fields].type = lir_imported_dependency(mapping, fields[sum_fields].type);
                    ++sum_fields;
                }
            }
        }
        if (shape.kind == XR_LIR_TYPE_CALLABLE) {
            callable.result = shape.element;
            callable.parameters = callable.parameter_count ? parameters : NULL;
            for (uint32_t p = 0; p < callable.parameter_count; ++p) {
                parameters[p] = data->callable.parameters[p];
                parameters[p].type = lir_imported_dependency(mapping, parameters[p].type);
            }
        }
        if (shape.kind == XR_LIR_TYPE_RECORD || shape.kind == XR_LIR_TYPE_TUPLE) {
            for (uint32_t f = 0; f < shape.count; ++f) {
                fields[f] = data->fields[f];
                fields[f].type = lir_imported_dependency(mapping, fields[f].type);
            }
        }
        record.arguments = record.argument_count ? arguments : NULL;
        for (uint32_t a = 0; a < record.argument_count; ++a) {
            arguments[a] = data->record.arguments[a];
            arguments[a].type = lir_imported_dependency(mapping, arguments[a].type);
        }
        XrLirType cell_element = data->cell_element;
        if (lir_nullable_array_shape(shape) && cell_element >= XR_LIR_TYPE_COUNT) {
            cell_element = lir_imported_dependency(mapping, cell_element);
            status = xr_lir_require_inline_array_element(target, cell_element);
            if (status != XR_LIR_OK) break;
        }
        status = lir_intern_type(target, shape,
            shape.kind == XR_LIR_TYPE_CALLABLE ? &callable : NULL, cell_element,
            lir_imported_dependency(mapping, data->class_parent),
            (shape.kind == XR_LIR_TYPE_RECORD || shape.kind == XR_LIR_TYPE_TUPLE) && shape.count ? fields :
                shape.kind == XR_LIR_TYPE_SUM ? fields : NULL,
            (LirNominalType){.identity=record.name ? &record : NULL, .allow_class_fields=data->class_payload_only},
            shape.kind == XR_LIR_TYPE_SUM ? variants : NULL, &mapping[n]);
    }
    if (status == XR_LIR_OK) *output = mapping[count - 1];
done:
    xr_free(parameters);
    xr_free(fields);
    xr_free(arguments);
    xr_free(variants);
    xr_free(mapping);
    xr_free(needed);
    return status;
}
