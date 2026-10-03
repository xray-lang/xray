/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdeclaration_manifest.c - Bounded schema admission without execution authority
 *
 * KEY CONCEPT:
 *   Preserve exact selectors and reject malformed or conflicting obligations.
 */
#include "xdeclaration_manifest.h"
#include <string.h>

typedef struct DeclarationRead {
    XrCompileResources *resources;
    XrDeclarationLimits remaining;
    XrDeclarationStatus status;
} DeclarationRead;

XR_FUNC void xr_compile_declaration_manifest_free(XrDeclarationManifest *manifest) {
    if (!manifest) return;
    for (uint32_t i = 0; i < manifest->count; ++i) {
        XrDeclarationRecord *record = &manifest->records[i];
        xr_compile_resources_free(record->module); xr_compile_resources_free(record->owner); xr_compile_resources_free(record->name);
        for (uint32_t p = 0; p < record->parameter_count; ++p) xr_compile_resources_free(record->parameters[p]);
        xr_compile_resources_free(record->parameters);
    }
    xr_compile_resources_free(manifest->records); xr_compile_resources_free(manifest);
}

static bool resource_status(DeclarationRead *read, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return true;
    if (read->status == XR_DECLARATION_INVALID)
        read->status = status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_DECLARATION_OUT_OF_MEMORY :
            status == XR_COMPILE_RESOURCE_BUDGET ? XR_DECLARATION_LIMIT : XR_DECLARATION_BAD_ARGUMENT;
    return false;
}
static bool work_units(DeclarationRead *read, uint64_t units) {
    return read->status == XR_DECLARATION_INVALID &&
        resource_status(read, xr_compile_resources_work(read->resources, units));
}
static bool work(DeclarationRead *read) { return work_units(read, 1); }
static void *allocate(DeclarationRead *read, size_t count, size_t size) {
    void *memory = NULL;
    if (read->status != XR_DECLARATION_INVALID) return NULL;
    if (!count || !size || count > SIZE_MAX / size) {
        read->status = XR_DECLARATION_LIMIT; return NULL;
    }
    resource_status(read, xr_compile_resources_calloc(read->resources, count, size, &memory));
    return memory;
}
static bool text_length(DeclarationRead *read, const char *text, size_t *length) {
    if (!text) return false;
    size_t n = 0;
    while (work(read)) {
        if (!text[n]) { *length = n; return true; }
        if (n == SIZE_MAX - 1) { read->status = XR_DECLARATION_LIMIT; return false; }
        ++n;
    }
    return false;
}
static bool same_text(DeclarationRead *read, const char *a, const char *b) {
    for (size_t i = 0; work_units(read, 2); ++i) {
        unsigned char left = (unsigned char)a[i], right = (unsigned char)b[i];
        if (left != right) return false;
        if (!left) return true;
    }
    return false;
}
static bool same_span(DeclarationRead *read, const char *a, const char *b, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (!work_units(read, 2) || a[i] != b[i]) return false;
    }
    return true;
}
static bool table_valid(XrTomlValue *table) {
    return table && table->type == XR_TOML_TABLE && table->as.table.count >= 0 &&
        (!table->as.table.count || table->as.table.members);
}
static XrTomlValue *lookup(DeclarationRead *read, XrTomlValue *table, const char *key) {
    size_t length = 0;
    if (read->status != XR_DECLARATION_INVALID || !table_valid(table) ||
        !text_length(read, key, &length)) return NULL;
    for (int i = 0; i < table->as.table.count; ++i) {
        if (!work(read)) return NULL;
        XrTomlMember *member = &table->as.table.members[i];
        if (member->key_length == length && member->key &&
            same_span(read, member->key, key, length)) return member->value;
    }
    return NULL;
}
static bool fields(DeclarationRead *read, XrTomlValue *table,
    const char *const *names, size_t count) {
    if (!work(read) || !table_valid(table)) return false;
    for (int i = 0; i < table->as.table.count; ++i) {
        XrTomlMember *member = &table->as.table.members[i];
        bool found = false;
        for (size_t n = 0; n < count; ++n) {
            if (!work(read)) return false;
            size_t length = 0;
            if (!text_length(read, names[n], &length)) return false;
            if (member->key_length == length && member->key && same_span(read, member->key, names[n], length)) found = true;
        }
        if (!found) return false;
    }
    return true;
}
static bool text(DeclarationRead *read, XrTomlValue *value, char **output) {
    if (!work(read) || !value || value->type != XR_TOML_STRING || !value->string_length ||
        value->string_length == SIZE_MAX || !value->as.string) return false;
    for (size_t i = 0; i < value->string_length; ++i) {
        if (!work(read) || !value->as.string[i]) return false;
    }
    char *copy = allocate(read, value->string_length + 1, 1);
    if (!copy) return false;
    if (!work_units(read, value->string_length)) { xr_compile_resources_free(copy); return false; }
    memcpy(copy, value->as.string, value->string_length);
    *output = copy; return true;
}
static bool logical_module(DeclarationRead *read, const char *path) {
    size_t length = 0, start = 0;
    if (!text_length(read, path, &length) || length < 4 ||
        !same_text(read, path + length - 3, ".xr")) return false;
    for (size_t i = 0; i <= length; ++i) {
        if (!work(read)) return false;
        unsigned char c = (unsigned char)path[i];
        if (c && (c < 32 || c == 127 || c == '\\' || c == ':')) return false;
        if (!c || c == '/') {
            size_t segment = i - start;
            if (!segment) return false;
            if (segment <= 2) {
                if (!work_units(read, segment)) return false;
                if (path[start] == '.' && (segment == 1 || path[start + 1] == '.')) return false;
            }
            start = i + 1;
        }
    }
    return true;
}
static bool parameters(DeclarationRead *read, XrTomlValue *array, XrDeclarationRecord *record) {
    if (!work(read)) return false;
    if (!array) return true;
    if (array->type != XR_TOML_ARRAY || array->as.array.count <= 0 || !array->as.array.items) return false;
    uint32_t count = (uint32_t)array->as.array.count;
    if (count > read->remaining.parameters) { read->status = XR_DECLARATION_LIMIT; return false; }
    record->parameters = allocate(read, count, sizeof(*record->parameters));
    if (!record->parameters) return false;
    record->parameter_count = count; read->remaining.parameters -= count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!text(read, array->as.array.items[i], &record->parameters[i])) return false;
        for (uint32_t j = 0; j < i; ++j) {
            if (!work(read) || same_text(read, record->parameters[i], record->parameters[j])) return false;
        }
    }
    return true;
}
static bool record_read(DeclarationRead *read, XrTomlValue *table, XrDeclarationRecord *record) {
    static const char *const allowed[] = {
        "module", "owner", "name", "no_suspend", "no_suspend_parameters"
    };
    if (!fields(read, table, allowed, sizeof(allowed) / sizeof(allowed[0])) ||
        !text(read, lookup(read, table, "module"), &record->module) || !logical_module(read, record->module) ||
        !text(read, lookup(read, table, "name"), &record->name)) return false;
    XrTomlValue *owner = lookup(read, table, "owner"), *promise = lookup(read, table, "no_suspend");
    if (owner && !text(read, owner, &record->owner)) return false;
    if (promise && (promise->type != XR_TOML_BOOL || !promise->as.boolean)) return false;
    record->no_suspend = promise != NULL;
    return parameters(read, lookup(read, table, "no_suspend_parameters"), record) &&
        (record->no_suspend || record->parameter_count);
}
static bool distinct(DeclarationRead *read, const XrDeclarationManifest *manifest, uint32_t index) {
    const XrDeclarationRecord *record = &manifest->records[index];
    for (uint32_t i = 0; i < index; ++i) {
        if (!work(read)) return false;
        const XrDeclarationRecord *prior = &manifest->records[i];
        if (same_text(read, record->module, prior->module) && same_text(read, record->name, prior->name) &&
            same_text(read, record->owner ? record->owner : "", prior->owner ? prior->owner : "")) return false;
    }
    return true;
}

static XrDeclarationStatus read_records(XrTomlValue *document,
    XrDeclarationLimits limits, XrDeclarationManifest **output, DeclarationRead *read) {
    if (!work(read) || !table_valid(document)) return read->status;
    XrTomlValue *section = lookup(read, document, "declarations");
    if (!section) return read->status != XR_DECLARATION_INVALID ? read->status : XR_DECLARATION_ABSENT;
    static const char *const allowed[] = {"version", "function"};
    if (!fields(read, section, allowed, 2)) return read->status;
    XrTomlValue *version = lookup(read, section, "version"), *records = lookup(read, section, "function");
    if (!version || version->type != XR_TOML_INTEGER || version->as.integer != 1 ||
        !records || records->type != XR_TOML_ARRAY || records->as.array.count <= 0 || !records->as.array.items) return read->status;
    uint32_t count = (uint32_t)records->as.array.count;
    if (count > limits.records) return XR_DECLARATION_LIMIT;
    XrDeclarationManifest *manifest = allocate(read, 1, sizeof(*manifest));
    if (!manifest) return read->status;
    manifest->records = allocate(read, count, sizeof(*manifest->records));
    if (!manifest->records) { xr_compile_declaration_manifest_free(manifest); return read->status; }
    manifest->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!work(read) || !record_read(read, records->as.array.items[i], &manifest->records[i]) ||
            read->status != XR_DECLARATION_INVALID ||
            !distinct(read, manifest, i) || read->status != XR_DECLARATION_INVALID) {
            xr_compile_declaration_manifest_free(manifest); return read->status;
        }
    }
    *output = manifest;
    return XR_DECLARATION_OK;
}

XR_FUNC XrDeclarationStatus xr_compile_declaration_manifest_read(
    XrCompileResources *resources, XrTomlValue *document,
    XrDeclarationLimits limits, XrDeclarationManifest **output) {
    if (!resources || !document || !output) return XR_DECLARATION_BAD_ARGUMENT;
    DeclarationRead read = {resources, limits, XR_DECLARATION_INVALID};
    return read_records(document, limits, output, &read);
}
