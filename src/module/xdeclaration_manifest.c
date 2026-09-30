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
#include "../base/xmalloc.h"
#include <string.h>

typedef struct DeclarationRead {
    XrDeclarationBudget remaining;
    XrDeclarationStatus status;
} DeclarationRead;

XR_FUNC void xr_declaration_manifest_free(XrDeclarationManifest *manifest) {
    if (!manifest) return;
    for (uint32_t i = 0; i < manifest->count; ++i) {
        XrDeclarationRecord *record = &manifest->records[i];
        xr_free(record->module); xr_free(record->owner); xr_free(record->name);
        for (uint32_t p = 0; p < record->parameter_count; ++p) xr_free(record->parameters[p]);
        xr_free(record->parameters);
    }
    xr_free(manifest->records); xr_free(manifest);
}

static bool work(DeclarationRead *read) {
    if (!read->remaining.work) { read->status = XR_DECLARATION_LIMIT; return false; }
    --read->remaining.work;
    return true;
}
static void *allocate(DeclarationRead *read, size_t count, size_t size) {
    if (count > SIZE_MAX / size || count * size > read->remaining.bytes) {
        read->status = XR_DECLARATION_LIMIT; return NULL;
    }
    void *result = xr_calloc(count, size);
    if (!result) { read->status = XR_DECLARATION_OUT_OF_MEMORY; return NULL; }
    read->remaining.bytes -= count * size;
    return result;
}
static XrTomlValue *lookup(DeclarationRead *read, XrTomlValue *table, const char *key) {
    size_t length = strlen(key);
    for (int i = 0; i < table->as.table.count; ++i) {
        if (!work(read)) return NULL;
        XrTomlMember *member = &table->as.table.members[i];
        if (member->key_length == length && !memcmp(member->key, key, length)) return member->value;
    }
    return NULL;
}
static bool fields(DeclarationRead *read, XrTomlValue *table,
    const char *const *names, size_t count) {
    if (!table || table->type != XR_TOML_TABLE) return false;
    for (int i = 0; i < table->as.table.count; ++i) {
        XrTomlMember *member = &table->as.table.members[i];
        bool found = false;
        for (size_t n = 0; n < count; ++n) {
            if (!work(read)) return false;
            size_t length = strlen(names[n]);
            if (member->key_length == length && !memcmp(member->key, names[n], length)) found = true;
        }
        if (!found) return false;
    }
    return true;
}
static bool text(DeclarationRead *read, XrTomlValue *value, char **output) {
    if (!value || value->type != XR_TOML_STRING || !value->string_length ||
        value->string_length == SIZE_MAX || !value->as.string ||
        memchr(value->as.string, 0, value->string_length)) return false;
    if (!work(read)) return false;
    char *copy = allocate(read, value->string_length + 1, 1);
    if (!copy) return false;
    memcpy(copy, value->as.string, value->string_length);
    *output = copy; return true;
}
static bool logical_module(const char *path) {
    size_t length = strlen(path), start = 0;
    if (length < 4 || strcmp(path + length - 3, ".xr")) return false;
    for (size_t i = 0; i <= length; ++i) {
        unsigned char c = (unsigned char)path[i];
        if (c && (c < 32 || c == 127 || c == '\\' || c == ':')) return false;
        if (!c || c == '/') {
            size_t segment = i - start;
            if (!segment || (segment == 1 && path[start] == '.') ||
                (segment == 2 && path[start] == '.' && path[start + 1] == '.')) return false;
            start = i + 1;
        }
    }
    return true;
}
static bool parameters(DeclarationRead *read, XrTomlValue *array, XrDeclarationRecord *record) {
    if (!array) return true;
    if (array->type != XR_TOML_ARRAY || array->as.array.count <= 0) return false;
    uint32_t count = (uint32_t)array->as.array.count;
    if (count > read->remaining.parameters) { read->status = XR_DECLARATION_LIMIT; return false; }
    record->parameters = allocate(read, count, sizeof(*record->parameters));
    if (!record->parameters) return false;
    record->parameter_count = count; read->remaining.parameters -= count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!text(read, array->as.array.items[i], &record->parameters[i])) return false;
        for (uint32_t j = 0; j < i; ++j) {
            if (!work(read) || !strcmp(record->parameters[i], record->parameters[j])) return false;
        }
    }
    return true;
}
static bool record_read(DeclarationRead *read, XrTomlValue *table, XrDeclarationRecord *record) {
    static const char *const allowed[] = {
        "module", "owner", "name", "no_suspend", "no_suspend_parameters"
    };
    if (!fields(read, table, allowed, sizeof(allowed) / sizeof(allowed[0])) ||
        !text(read, lookup(read, table, "module"), &record->module) || !logical_module(record->module) ||
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
        if (!strcmp(record->module, prior->module) && !strcmp(record->name, prior->name) &&
            !strcmp(record->owner ? record->owner : "", prior->owner ? prior->owner : "")) return false;
    }
    return true;
}

static XrDeclarationStatus read_records(XrTomlValue *document,
    XrDeclarationBudget budget, XrDeclarationManifest **output, DeclarationRead *read) {
    if (output) *output = NULL;
    if (!output || !document || document->type != XR_TOML_TABLE) return XR_DECLARATION_INVALID;
    XrTomlValue *section = lookup(read, document, "declarations");
    if (!section) return read->status == XR_DECLARATION_LIMIT ? read->status : XR_DECLARATION_ABSENT;
    static const char *const allowed[] = {"version", "function"};
    if (!fields(read, section, allowed, 2)) return read->status;
    XrTomlValue *version = lookup(read, section, "version"), *records = lookup(read, section, "function");
    if (!version || version->type != XR_TOML_INTEGER || version->as.integer != 1 ||
        !records || records->type != XR_TOML_ARRAY || records->as.array.count <= 0) return read->status;
    uint32_t count = (uint32_t)records->as.array.count;
    if (count > budget.records) return XR_DECLARATION_LIMIT;
    XrDeclarationManifest *manifest = allocate(read, 1, sizeof(*manifest));
    if (!manifest) return read->status;
    manifest->records = allocate(read, count, sizeof(*manifest->records));
    if (!manifest->records) { xr_declaration_manifest_free(manifest); return read->status; }
    manifest->count = count;
    for (uint32_t i = 0; i < count; ++i) {
        if (!work(read) || !record_read(read, records->as.array.items[i], &manifest->records[i]) ||
            read->status != XR_DECLARATION_INVALID ||
            !distinct(read, manifest, i)) {
            xr_declaration_manifest_free(manifest); return read->status;
        }
    }
    *output = manifest;
    return XR_DECLARATION_OK;
}

XR_FUNC XrDeclarationStatus xr_declaration_manifest_read(XrTomlValue *document,
    XrDeclarationBudget budget, XrDeclarationManifest **output, size_t *work_used) {
    DeclarationRead read = {budget, XR_DECLARATION_INVALID};
    XrDeclarationStatus status = read_records(document, budget, output, &read);
    if (work_used) *work_used = (size_t)budget.work - read.remaining.work;
    return status;
}
