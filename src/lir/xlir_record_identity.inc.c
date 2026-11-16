/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_record_identity.inc.c - Owned nominal record identities and field indices
 *
 * KEY CONCEPT:
 *   Physical boxing preserves the declaration and its actual arguments.
 *   Field-name indices contain ordinals and never reorder the payload.
 */
#include "shared/xr_utf8_core.h"

static bool lir_record_text_valid(const char *text, uint32_t length) {
    return text && length && !memchr(text, 0, length) &&
        xr_utf8_core_scan_strict((const uint8_t *)text, length).error == XR_UTF8_OK;
}

static bool lir_record_argument_valid(const XrLirModule *m, XrLirRecordArgument argument, uint32_t earlier) {
    XrLirType type = argument.type;
    if (!xr_lir_internal_type_valid(m, type, true) || type >= XR_LIR_TYPE_COUNT + earlier) return false;
    if (type == XR_LIR_ARRAY)
        return argument.array_element == XR_LIR_I64 || argument.array_element == XR_LIR_STRING ||
               argument.array_element == XR_LIR_U8 || argument.array_element == XR_LIR_U64;
    if (argument.array_element) return false;
    if (type < XR_LIR_TYPE_COUNT)
        return lir_pod_scalar(type) < XR_POD_SCALAR_COUNT || type == XR_LIR_STRING || type == XR_LIR_MAP_STRING_I64;
    return m->types[type - XR_LIR_TYPE_COUNT].shape.kind != XR_LIR_TYPE_CELL;
}

static bool lir_record_identity_valid(const XrLirModule *m, const XrLirRecordIdentity *identity, uint32_t earlier) {
    if (!identity || !lir_record_text_valid(identity->module, identity->module_length) ||
        !lir_record_text_valid(identity->name, identity->name_length) || identity->exported > 1 ||
        (identity->flags & ~XR_LIR_RECORD_MODULE_LITERAL) ||
        ((identity->argument_count != 0) != (identity->arguments != NULL))) return false;
    for (uint32_t a = 0; a < identity->argument_count; ++a)
        if (!lir_record_argument_valid(m, identity->arguments[a], earlier)) return false;
    return true;
}

static bool lir_record_declaration_equal(const XrLirRecordIdentity *a, const XrLirRecordIdentity *b) {
    return a->module_length == b->module_length && a->name_length == b->name_length &&
        !memcmp(a->module, b->module, a->module_length) && !memcmp(a->name, b->name, a->name_length);
}

static bool lir_record_identity_equal(const XrLirRecordIdentity *a, const XrLirRecordIdentity *b) {
    if (!lir_record_declaration_equal(a, b) || a->argument_count != b->argument_count) return false;
    for (uint32_t p = 0; p < a->argument_count; ++p)
        if (a->arguments[p].type != b->arguments[p].type ||
            a->arguments[p].array_element != b->arguments[p].array_element) return false;
    return true;
}

static uint32_t lir_record_identity_hash(const XrLirRecordIdentity *identity) {
    uint32_t hash = xr_hash_bytes(identity->module, identity->module_length);
    hash = xr_hash_bytes_update(hash, identity->name, identity->name_length);
    for (uint32_t p = 0; p < identity->argument_count; ++p) {
        uint32_t argument[] = {hash, identity->arguments[p].type, identity->arguments[p].array_element};
        hash = xr_hash_bytes(argument, sizeof(argument));
    }
    return hash;
}

static bool lir_record_schema_equal(const XrLirRecordField *a, const XrLirRecordField *b,
                                    uint32_t count, bool actual) {
    for (uint32_t f = 0; f < count; ++f)
        if (a[f].name_length != b[f].name_length || a[f].flags != b[f].flags ||
            (actual && a[f].type != b[f].type) || memcmp(a[f].name, b[f].name, a[f].name_length)) return false;
    return true;
}

static bool lir_record_declaration_consistent(const XrLirModule *m, const XrLirRecordIdentity *identity,
                                             const XrLirRecordField *fields, uint32_t count) {
    for (uint32_t t = 0; t < m->type_count; ++t) {
        XrLirTypeData data = m->types[t];
        if (data.shape.kind != XR_LIR_TYPE_RECORD || !lir_record_declaration_equal(&data.record, identity)) continue;
        if (data.record.argument_count != identity->argument_count || data.record.exported != identity->exported ||
            data.record.flags != identity->flags || data.shape.count != count ||
            !lir_record_schema_equal(data.fields, fields, count, false)) return false;
    }
    return true;
}

static char *lir_record_text_copy(XrLirModule *m, const char *text, uint32_t length) {
    size_t bytes = (size_t)length + 1;
    if (bytes <= length) { lir_fail(m, XR_LIR_INVALID_ARGUMENT); return NULL; }
    char *copy = lir_allocate(m, bytes, 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}

static void lir_record_identity_free(XrLirRecordIdentity *identity) {
    xr_free((void *)identity->module); xr_free((void *)identity->name); xr_free((void *)identity->arguments);
    *identity = (XrLirRecordIdentity){0};
}

static bool lir_record_identity_copy(XrLirModule *m, const XrLirRecordIdentity *source,
                                      XrLirRecordIdentity *output) {
    XrLirRecordIdentity identity = *source;
    identity.module = lir_record_text_copy(m, source->module, source->module_length);
    identity.name = NULL; identity.arguments = NULL;
    if (identity.module) identity.name = lir_record_text_copy(m, source->name, source->name_length);
    if (identity.name && source->argument_count)
        identity.arguments = lir_copy(m, source->arguments, source->argument_count, sizeof(*identity.arguments));
    if (!identity.module || !identity.name || (source->argument_count && !identity.arguments)) {
        lir_record_identity_free(&identity); return false;
    }
    *output = identity;
    return true;
}

static bool lir_record_name_index(XrLirModule *m, const XrLirRecordField *fields, uint32_t count,
                                 uint32_t **output, uint32_t *capacity) {
    if (!count) { *output = NULL; *capacity = 0; return true; }
    if (count > UINT32_MAX / 2) { lir_fail(m, XR_LIR_INVALID_ARGUMENT); return false; }
    uint32_t size = 4;
    while (size / 2 < count) {
        if (size > UINT32_MAX / 2) { lir_fail(m, XR_LIR_INVALID_ARGUMENT); return false; }
        size *= 2;
    }
    uint32_t *index = lir_allocate(m, size, sizeof(*index));
    if (!index) return false;
    memset(index, 0, (size_t)size * sizeof(*index));
    for (uint32_t f = 0; f < count; ++f) {
        uint32_t slot = xr_hash_bytes(fields[f].name, fields[f].name_length) & (size - 1);
        while (index[slot]) {
            XrLirRecordField prior = fields[index[slot] - 1];
            if (prior.name_length == fields[f].name_length && !memcmp(prior.name, fields[f].name, prior.name_length)) {
                xr_free(index); lir_fail(m, XR_LIR_INVALID_ARGUMENT); return false;
            }
            slot = (slot + 1) & (size - 1);
        }
        index[slot] = f + 1;
    }
    *output = index; *capacity = size;
    return true;
}

static bool lir_record_name_index_valid(const XrLirTypeData *data) {
    uint32_t count = data->shape.count, capacity = data->record_name_capacity;
    if (!count) return !capacity && !data->record_names;
    if (!data->record_names || !capacity || (capacity & (capacity - 1)) || count > capacity / 2) return false;
    uint32_t entries = 0;
    for (uint32_t s = 0; s < capacity; ++s) {
        if (data->record_names[s] > count) return false;
        entries += data->record_names[s] != 0;
    }
    if (entries != count) return false;
    for (uint32_t f = 0; f < count; ++f) {
        XrLirRecordField field = data->fields[f];
        uint32_t slot = xr_hash_bytes(field.name, field.name_length) & (capacity - 1), examined = 0;
        while (data->record_names[slot] && examined++ < capacity) {
            XrLirRecordField found = data->fields[data->record_names[slot] - 1];
            if (found.name_length == field.name_length && !memcmp(found.name, field.name, field.name_length)) break;
            slot = (slot + 1) & (capacity - 1);
        }
        if (data->record_names[slot] != f + 1) return false;
    }
    return true;
}
