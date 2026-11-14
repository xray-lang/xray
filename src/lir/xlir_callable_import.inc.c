/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_callable_import.inc.c - Iterative receiving of callable dependencies
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

static XrLirStatus lir_import_callable(XrLirModule *target, const XrLirModule *source,
    XrLirType type, XrLirType *output) {
    if (target == source) { *output = type; return XR_LIR_OK; }
    uint32_t count = type - XR_LIR_TYPE_COUNT + 1;
    uint8_t *needed = lir_allocate(target, count, sizeof(*needed));
    XrLirType *mapping = lir_allocate(target, count, sizeof(*mapping));
    XrLirCallableParameter *parameters = NULL;
    XrLirRecordField *fields = NULL;
    XrLirStatus status = target->failure;
    if (!needed || !mapping) goto done;
    memset(needed, 0, count * sizeof(*needed));
    memset(mapping, 0, count * sizeof(*mapping));
    needed[count - 1] = 1;
    uint32_t largest = 0, largest_record = 0;
    for (uint32_t n = count; n-- > 0;) {
        if (!needed[n]) continue;
        const XrLirTypeData *data = &source->types[n];
        lir_import_dependency(needed, data->shape.element);
        if (data->shape.kind == XR_LIR_TYPE_RECORD) {
            if (data->shape.count > largest_record) largest_record = data->shape.count;
            for (uint32_t f = 0; f < data->shape.count; ++f) lir_import_dependency(needed, data->fields[f].type);
        }
        if (data->shape.kind != XR_LIR_TYPE_CALLABLE) continue;
        if (data->callable.parameter_count > largest) largest = data->callable.parameter_count;
        for (uint32_t p = 0; p < data->callable.parameter_count; ++p)
            lir_import_dependency(needed, data->callable.parameters[p].type);
    }
    parameters = largest ? lir_allocate(target, largest, sizeof(*parameters)) : NULL;
    if (largest && !parameters) { status = target->failure; goto done; }
    fields = largest_record ? lir_allocate(target, largest_record, sizeof(*fields)) : NULL;
    if (largest_record && !fields) { status = target->failure; goto done; }
    for (uint32_t n = 0; n < count && status == XR_LIR_OK; ++n) {
        if (!needed[n]) continue;
        const XrLirTypeData *data = &source->types[n];
        XrLirTypeView shape = data->shape;
        shape.element = lir_imported_dependency(mapping, shape.element);
        XrLirCallableType callable = data->callable;
        if (shape.kind == XR_LIR_TYPE_CALLABLE) {
            callable.result = shape.element;
            callable.parameters = callable.parameter_count ? parameters : NULL;
            for (uint32_t p = 0; p < callable.parameter_count; ++p) {
                parameters[p] = data->callable.parameters[p];
                parameters[p].type = lir_imported_dependency(mapping, parameters[p].type);
            }
        }
        if (shape.kind == XR_LIR_TYPE_RECORD)
            for (uint32_t f = 0; f < shape.count; ++f) {
                fields[f] = data->fields[f];
                fields[f].type = lir_imported_dependency(mapping, fields[f].type);
            }
        status = lir_intern_type(target, shape,
            shape.kind == XR_LIR_TYPE_CALLABLE ? &callable : NULL, data->cell_element,
            shape.kind == XR_LIR_TYPE_RECORD && shape.count ? fields : NULL, &mapping[n]);
    }
    if (status == XR_LIR_OK) *output = mapping[count - 1];
done:
    xr_free(parameters);
    xr_free(fields);
    xr_free(mapping);
    xr_free(needed);
    return status;
}
