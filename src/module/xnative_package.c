/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_package.c - Native package manifest parser and verifier
 */

#include "xnative_package.h"
#include "../base/xfileio.h"
#include "../base/xmalloc.h"
#include "../shared/xr_crypto_core.h"
#include "../runtime/value/xstruct_layout.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XR_NATIVE_FNV_OFFSET UINT64_C(1469598103934665603)
#define XR_NATIVE_FNV_PRIME UINT64_C(1099511628211)

static bool native_valid_c_identifier(const char *name);

static uint64_t native_hash_bytes(uint64_t hash, const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char *) data;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= XR_NATIVE_FNV_PRIME;
    }
    return hash;
}

static uint64_t native_hash_text(uint64_t hash, const char *text) {
    if (!text)
        return native_hash_bytes(hash, "\xff", 1);
    hash = native_hash_bytes(hash, text, strlen(text));
    return native_hash_bytes(hash, "\0", 1);
}

static bool native_fail(XrNativePackagePlan *plan, const char *fmt, ...) {
    char message[768];
    va_list ap;
    if (!plan)
        return false;
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    if (!plan->error)
        plan->error = xr_strdup(message);
    plan->valid = false;
    return false;
}

static char *native_dup_string(XrTomlValue *table, const char *key) {
    const char *value = xtoml_get_string(table, key);
    return value ? xr_strdup(value) : NULL;
}

static bool native_key_is(const char *key, const char *const *allowed, size_t count) {
    if (!key)
        return false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(key, allowed[i]) == 0)
            return true;
    }
    return false;
}

static bool native_text_is(const char *text, const char *const *allowed, size_t count) {
    return native_key_is(text, allowed, count);
}

static bool native_validate_keys(XrNativePackagePlan *plan, XrTomlValue *table, const char *where,
                                 const char *const *allowed, size_t allowed_count) {
    if (!table || table->type != XR_TOML_TABLE)
        return native_fail(plan, "E-NATIVE-SCHEMA: %s must be a table", where);
    for (int i = 0; i < table->as.table.count; i++) {
        const char *key = table->as.table.members[i].key;
        if (!native_key_is(key, allowed, allowed_count))
            return native_fail(plan, "E-NATIVE-SCHEMA: unsupported field %s.%s", where,
                               key ? key : "?");
    }
    return true;
}

static void native_free_string_array(char **items, uint32_t count) {
    if (!items)
        return;
    for (uint32_t i = 0; i < count; i++)
        xr_free(items[i]);
    xr_free(items);
}

static bool native_parse_string_array(XrNativePackagePlan *plan, XrTomlValue *table,
                                      const char *key, bool required, char ***out_items,
                                      uint32_t *out_count, const char *where) {
    XrTomlValue *array = xtoml_get_array(table, key);
    *out_items = NULL;
    *out_count = 0;
    if (!array) {
        if (required)
            return native_fail(plan, "E-NATIVE-SCHEMA: %s.%s is required", where, key);
        return true;
    }
    int count = xtoml_array_len(array);
    if (required && count == 0)
        return native_fail(plan, "E-NATIVE-SCHEMA: %s.%s cannot be empty", where, key);
    if (count <= 0)
        return true;
    char **items = (char **) xr_calloc((size_t) count, sizeof(char *));
    if (!items)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate %s.%s", where, key);
    for (int i = 0; i < count; i++) {
        XrTomlValue *item = xtoml_array_get(array, i);
        if (!item || item->type != XR_TOML_STRING || !item->as.string || !item->as.string[0]) {
            native_free_string_array(items, (uint32_t) count);
            return native_fail(plan, "E-NATIVE-SCHEMA: %s.%s[%d] must be a non-empty string", where,
                               key, i);
        }
        items[i] = xr_strdup(item->as.string);
        if (!items[i]) {
            native_free_string_array(items, (uint32_t) count);
            return native_fail(plan, "E-NATIVE-RESOURCE: cannot copy %s.%s[%d]", where, key, i);
        }
    }
    *out_items = items;
    *out_count = (uint32_t) count;
    return true;
}

static bool native_safe_relative_path(const char *path) {
    const char *segment;
    if (!path || !path[0] || path[0] == '/' || path[0] == '\\')
        return false;
    if (isalpha((unsigned char) path[0]) && path[1] == ':')
        return false;
    segment = path;
    for (const char *p = path;; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            size_t len = (size_t) (p - segment);
            if (len == 0 || (len == 1 && segment[0] == '.') ||
                (len == 2 && segment[0] == '.' && segment[1] == '.'))
                return false;
            if (*p == '\0')
                break;
            segment = p + 1;
        }
    }
    return true;
}

static bool native_path_belongs_to_root(const char *root, const char *path) {
    size_t root_len;
    if (!root || !path)
        return false;
    root_len = strlen(root);
    return strncmp(root, path, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/' || path[root_len] == '\\');
}

static char *native_resolve_existing_path(XrNativePackagePlan *plan, const char *relative,
                                          const char *where) {
    if (!native_safe_relative_path(relative)) {
        native_fail(plan, "E-NATIVE-PATH: %s must be a normalized package-relative path: %s", where,
                    relative ? relative : "");
        return NULL;
    }
    char *joined = xr_path_join(plan->root, relative);
    char *resolved = joined ? xr_realpath(joined) : NULL;
    xr_free(joined);
    if (!resolved) {
        native_fail(plan, "E-NATIVE-PATH: %s does not exist: %s", where, relative);
        return NULL;
    }
    if (!native_path_belongs_to_root(plan->root, resolved)) {
        native_fail(plan, "E-NATIVE-PATH: %s escapes the package root: %s", where, relative);
        xr_free(resolved);
        return NULL;
    }
    return resolved;
}

static char *native_resolve_output_path(XrNativePackagePlan *plan, const char *relative,
                                        const char *where) {
    if (!relative)
        return NULL;
    if (!native_safe_relative_path(relative)) {
        native_fail(plan, "E-NATIVE-PATH: %s must be a normalized package-relative path: %s", where,
                    relative);
        return NULL;
    }
    return xr_path_join(plan->root, relative);
}

static bool native_sha256_file(const char *path, char output[65]) {
    size_t size = 0;
    char *content = xr_file_read_all(path, "rb", &size);
    uint8_t digest[32];
    if (!content)
        return false;
    xr_sha256((const uint8_t *) content, size, digest);
    xr_free(content);
    xr_bytes_to_hex(digest, sizeof(digest), output);
    output[64] = '\0';
    return true;
}

static bool native_valid_sha256(const char *text) {
    if (!text || strlen(text) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        if (!isdigit((unsigned char) text[i]) && !(text[i] >= 'a' && text[i] <= 'f'))
            return false;
    }
    return true;
}

static bool native_valid_define(const char *text) {
    const char *p = text;
    if (!p || !(isalpha((unsigned char) *p) || *p == '_'))
        return false;
    p++;
    while (isalnum((unsigned char) *p) || *p == '_')
        p++;
    if (*p == '\0')
        return true;
    if (*p++ != '=')
        return false;
    for (; *p; p++) {
        if (*p == '\n' || *p == '\r' || (unsigned char) *p < 0x20)
            return false;
    }
    return true;
}

static bool native_parse_audit(XrNativePackagePlan *plan, const char *value) {
    if (value && strcmp(value, "shipping") == 0)
        plan->audit_mode = XR_NATIVE_AUDIT_SHIPPING;
    else if (value && strcmp(value, "exploratory") == 0)
        plan->audit_mode = XR_NATIVE_AUDIT_EXPLORATORY;
    else
        return native_fail(plan,
                           "E-NATIVE-SCHEMA: native.audit_mode must be shipping or exploratory");
    return true;
}

static bool native_parse_vm_policy(XrNativePackagePlan *plan, const char *value) {
    if (value && strcmp(value, "verified-dynamic") == 0)
        plan->vm_policy = XR_NATIVE_VM_VERIFIED_DYNAMIC;
    else if (value && strcmp(value, "unsupported") == 0)
        plan->vm_policy = XR_NATIVE_VM_UNSUPPORTED;
    else
        return native_fail(plan,
                           "E-NATIVE-SCHEMA: native.vm must be verified-dynamic or unsupported");
    return true;
}

static XrNativeUnitKind native_unit_kind(const char *value) {
    if (!value)
        return 0;
    if (strcmp(value, "c") == 0)
        return XR_NATIVE_UNIT_C;
    if (strcmp(value, "asm") == 0)
        return XR_NATIVE_UNIT_ASM;
    if (strcmp(value, "object") == 0)
        return XR_NATIVE_UNIT_OBJECT;
    if (strcmp(value, "static-library") == 0)
        return XR_NATIVE_UNIT_STATIC_LIBRARY;
    if (strcmp(value, "dynamic-library") == 0)
        return XR_NATIVE_UNIT_DYNAMIC_LIBRARY;
    if (strcmp(value, "platform") == 0)
        return XR_NATIVE_UNIT_PLATFORM;
    return 0;
}

static XrNativeSymbolKind native_symbol_kind(const char *value) {
    if (value && strcmp(value, "function") == 0)
        return XR_NATIVE_SYMBOL_FUNCTION;
    if (value && strcmp(value, "address") == 0)
        return XR_NATIVE_SYMBOL_ADDRESS;
    return 0;
}

static XrNativeParamAccess native_access(const char *value) {
    if (value && strcmp(value, "none") == 0)
        return XR_NATIVE_ACCESS_NONE;
    if (value && strcmp(value, "read") == 0)
        return XR_NATIVE_ACCESS_READ;
    if (value && strcmp(value, "write") == 0)
        return XR_NATIVE_ACCESS_WRITE;
    if (value && strcmp(value, "readwrite") == 0)
        return XR_NATIVE_ACCESS_READWRITE;
    return (XrNativeParamAccess) -1;
}

static XrNativeEscape native_escape(const char *value) {
    if (value && strcmp(value, "noescape") == 0)
        return XR_NATIVE_ESCAPE_NOESCAPE;
    if (value && strcmp(value, "borrow") == 0)
        return XR_NATIVE_ESCAPE_BORROW;
    if (value && strcmp(value, "retain") == 0)
        return XR_NATIVE_ESCAPE_RETAIN;
    if (value && strcmp(value, "consume") == 0)
        return XR_NATIVE_ESCAPE_CONSUME;
    return XR_NATIVE_ESCAPE_UNSPECIFIED;
}

static XrNativeOwnership native_ownership(const char *value) {
    if (value && strcmp(value, "value") == 0)
        return XR_NATIVE_OWNERSHIP_VALUE;
    if (value && strcmp(value, "borrowed") == 0)
        return XR_NATIVE_OWNERSHIP_BORROWED;
    if (value && strcmp(value, "owned") == 0)
        return XR_NATIVE_OWNERSHIP_OWNED;
    if (value && strcmp(value, "none") == 0)
        return XR_NATIVE_OWNERSHIP_NONE;
    return XR_NATIVE_OWNERSHIP_UNSPECIFIED;
}

static XrNativeOutputState native_output(const char *value) {
    if (!value || strcmp(value, "none") == 0)
        return XR_NATIVE_OUTPUT_NONE;
    if (strcmp(value, "complete") == 0)
        return XR_NATIVE_OUTPUT_COMPLETE;
    if (strcmp(value, "partial") == 0)
        return XR_NATIVE_OUTPUT_PARTIAL;
    return (XrNativeOutputState) -1;
}

static bool native_parse_units(XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const unit_keys[] = {
        "name",     "kind",         "sources",    "source_hashes", "include_dirs",
        "defines",  "system_links", "c_standard", "optimization",  "visibility",
        "warnings", "cpu_feature",  "output",     "purpose",
    };
    XrTomlValue *array = xtoml_get_array(native, "unit");
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(plan, "E-NATIVE-SCHEMA: native.unit cannot be empty");
    plan->units = (XrNativeUnit *) xr_calloc((size_t) count, sizeof(XrNativeUnit));
    if (!plan->units)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate native units");
    plan->unit_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        char where[64];
        snprintf(where, sizeof(where), "native.unit[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeUnit *unit = &plan->units[i];
        if (!native_validate_keys(plan, table, where, unit_keys,
                                  sizeof(unit_keys) / sizeof(unit_keys[0])))
            return false;
        unit->name = native_dup_string(table, "name");
        unit->kind = native_unit_kind(xtoml_get_string(table, "kind"));
        unit->language_standard = native_dup_string(table, "c_standard");
        unit->optimization = native_dup_string(table, "optimization");
        unit->visibility = native_dup_string(table, "visibility");
        unit->warning_policy = native_dup_string(table, "warnings");
        unit->cpu_feature = native_dup_string(table, "cpu_feature");
        unit->purpose = native_dup_string(table, "purpose");
        if (!unit->name || !unit->name[0] || !unit->kind || !unit->purpose || !unit->purpose[0])
            return native_fail(
                plan, "E-NATIVE-SCHEMA: %s requires name, supported kind, and purpose", where);
        for (int j = 0; j < i; j++) {
            if (strcmp(plan->units[j].name, unit->name) == 0)
                return native_fail(plan, "E-NATIVE-SCHEMA: duplicate native unit '%s'", unit->name);
        }
        if (unit->kind != XR_NATIVE_UNIT_PLATFORM) {
            if (!native_parse_string_array(plan, table, "sources", true, &unit->source_relpaths,
                                           &unit->source_count, where))
                return false;
            uint32_t hash_count = 0;
            if (!native_parse_string_array(plan, table, "source_hashes", true, &unit->source_hashes,
                                           &hash_count, where))
                return false;
            if (hash_count != unit->source_count)
                return native_fail(plan, "E-NATIVE-SCHEMA: %s.source_hashes must match sources",
                                   where);
            unit->sources = (char **) xr_calloc((size_t) unit->source_count, sizeof(char *));
            if (!unit->sources)
                return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate %s.sources", where);
            uint64_t fingerprint = XR_NATIVE_FNV_OFFSET;
            for (uint32_t si = 0; si < unit->source_count; si++) {
                char source_where[96];
                char actual[65];
                snprintf(source_where, sizeof(source_where), "%s.sources[%u]", where, si);
                unit->sources[si] =
                    native_resolve_existing_path(plan, unit->source_relpaths[si], source_where);
                if (!unit->sources[si])
                    return false;
                if (!native_valid_sha256(unit->source_hashes[si]))
                    return native_fail(plan,
                                       "E-NATIVE-SCHEMA: %s.source_hashes[%u] is not lower-case "
                                       "SHA-256",
                                       where, si);
                if (!native_sha256_file(unit->sources[si], actual))
                    return native_fail(plan, "E-NATIVE-HASH: cannot hash %s",
                                       unit->source_relpaths[si]);
                if (strcmp(actual, unit->source_hashes[si]) != 0)
                    return native_fail(plan,
                                       "E-NATIVE-HASH-MISMATCH: %s differs from audited hash "
                                       "(expected %s, actual %s)",
                                       unit->source_relpaths[si], unit->source_hashes[si], actual);
                fingerprint = native_hash_text(fingerprint, unit->source_relpaths[si]);
                fingerprint = native_hash_text(fingerprint, actual);
            }
            unit->fingerprint = fingerprint ? fingerprint : 1;
        }
        uint32_t rel_include_count = 0;
        char **rel_includes = NULL;
        if (!native_parse_string_array(plan, table, "include_dirs", false, &rel_includes,
                                       &rel_include_count, where))
            return false;
        if (rel_include_count > 0) {
            unit->include_dirs = (char **) xr_calloc((size_t) rel_include_count, sizeof(char *));
            if (!unit->include_dirs) {
                native_free_string_array(rel_includes, rel_include_count);
                return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate include dirs");
            }
            unit->include_dir_count = rel_include_count;
            for (uint32_t di = 0; di < rel_include_count; di++) {
                char include_where[96];
                snprintf(include_where, sizeof(include_where), "%s.include_dirs[%u]", where, di);
                unit->include_dirs[di] =
                    native_resolve_existing_path(plan, rel_includes[di], include_where);
                if (!unit->include_dirs[di]) {
                    native_free_string_array(rel_includes, rel_include_count);
                    return false;
                }
            }
        }
        native_free_string_array(rel_includes, rel_include_count);
        if (!native_parse_string_array(plan, table, "defines", false, &unit->defines,
                                       &unit->define_count, where) ||
            !native_parse_string_array(plan, table, "system_links", false, &unit->system_links,
                                       &unit->system_link_count, where))
            return false;
        for (uint32_t di = 0; di < unit->define_count; di++) {
            if (!native_valid_define(unit->defines[di]))
                return native_fail(plan, "E-NATIVE-FLAG: unsafe define in %s: %s", where,
                                   unit->defines[di]);
        }
        const char *output = xtoml_get_string(table, "output");
        if (output) {
            char output_where[80];
            snprintf(output_where, sizeof(output_where), "%s.output", where);
            unit->output = native_resolve_output_path(plan, output, output_where);
            if (!unit->output)
                return false;
        }
        if ((unit->kind == XR_NATIVE_UNIT_C || unit->kind == XR_NATIVE_UNIT_ASM) &&
            (!unit->output || !unit->output[0]))
            return native_fail(plan, "E-NATIVE-SCHEMA: %s requires an explicit derived output path",
                               where);
        if ((unit->kind == XR_NATIVE_UNIT_OBJECT || unit->kind == XR_NATIVE_UNIT_STATIC_LIBRARY ||
             unit->kind == XR_NATIVE_UNIT_DYNAMIC_LIBRARY) &&
            unit->source_count != 1)
            return native_fail(plan,
                               "E-NATIVE-SCHEMA: %s prebuilt unit requires exactly one audited "
                               "source artifact",
                               where);
        if (unit->kind == XR_NATIVE_UNIT_C &&
            (!unit->language_standard || (strcmp(unit->language_standard, "c11") != 0 &&
                                          strcmp(unit->language_standard, "c17") != 0 &&
                                          strcmp(unit->language_standard, "c23") != 0)))
            return native_fail(plan, "E-NATIVE-SCHEMA: %s.c_standard must be c11, c17, or c23",
                               where);
        if (!unit->optimization ||
            (strcmp(unit->optimization, "none") != 0 && strcmp(unit->optimization, "size") != 0 &&
             strcmp(unit->optimization, "release") != 0))
            return native_fail(
                plan, "E-NATIVE-FLAG: %s.optimization must be none, size, or release", where);
        if (!unit->visibility ||
            (strcmp(unit->visibility, "hidden") != 0 && strcmp(unit->visibility, "default") != 0))
            return native_fail(plan, "E-NATIVE-FLAG: %s.visibility must be hidden or default",
                               where);
        if (!unit->warning_policy || (strcmp(unit->warning_policy, "strict") != 0 &&
                                      strcmp(unit->warning_policy, "system") != 0))
            return native_fail(plan, "E-NATIVE-FLAG: %s.warnings must be strict or system", where);
        if (unit->cpu_feature && strcmp(unit->cpu_feature, "baseline") != 0)
            return native_fail(
                plan, "E-NATIVE-FLAG: %s.cpu_feature must be the sealed value baseline", where);
        {
            uint64_t fingerprint = unit->fingerprint ? unit->fingerprint : XR_NATIVE_FNV_OFFSET;
            fingerprint = native_hash_bytes(fingerprint, &unit->kind, sizeof(unit->kind));
            for (uint32_t di = 0; di < unit->include_dir_count; di++)
                fingerprint = native_hash_text(fingerprint, unit->include_dirs[di]);
            for (uint32_t di = 0; di < unit->define_count; di++)
                fingerprint = native_hash_text(fingerprint, unit->defines[di]);
            for (uint32_t li = 0; li < unit->system_link_count; li++)
                fingerprint = native_hash_text(fingerprint, unit->system_links[li]);
            fingerprint = native_hash_text(fingerprint, unit->language_standard);
            fingerprint = native_hash_text(fingerprint, unit->optimization);
            fingerprint = native_hash_text(fingerprint, unit->visibility);
            fingerprint = native_hash_text(fingerprint, unit->warning_policy);
            fingerprint = native_hash_text(fingerprint, unit->cpu_feature);
            unit->fingerprint = fingerprint ? fingerprint : 1;
        }
    }
    return true;
}

static bool native_parse_param_contract(XrNativePackagePlan *plan, XrTomlValue *table,
                                        XrNativeParamContract *param, uint32_t expected_index,
                                        const char *where) {
    static const char *const keys[] = {
        "index",
        "access",
        "nullable",
        "length_from",
        "escape",
        "ownership",
        "output",
        "descriptor_rebind",
        "may_relocate",
        "may_shorten",
        "invalidates_views",
    };
    if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
        return false;
    int64_t index = xtoml_get_int_or(table, "index", -1);
    if (index < 0 || (uint32_t) index != expected_index)
        return native_fail(plan, "E-NATIVE-CONTRACT: %s.index must be %u", where, expected_index);
    param->index = expected_index;
    param->access = native_access(xtoml_get_string(table, "access"));
    param->escape = native_escape(xtoml_get_string(table, "escape"));
    param->ownership = native_ownership(xtoml_get_string(table, "ownership"));
    param->output = native_output(xtoml_get_string(table, "output"));
    XrTomlValue *nullable = xtoml_get(table, "nullable");
    if ((int) param->access < 0 || !param->escape || !param->ownership || (int) param->output < 0 ||
        !nullable || nullable->type != XR_TOML_BOOL)
        return native_fail(plan,
                           "E-NATIVE-CONTRACT: %s requires typed access, nullable, escape, "
                           "ownership, and output",
                           where);
    param->nullable = nullable->as.boolean;
    param->length_from = (int32_t) xtoml_get_int_or(table, "length_from", -1);
    param->descriptor_rebind = xtoml_get_bool_or(table, "descriptor_rebind", false);
    param->may_relocate = xtoml_get_bool_or(table, "may_relocate", false);
    param->may_shorten = xtoml_get_bool_or(table, "may_shorten", false);
    param->invalidates_views = xtoml_get_bool_or(table, "invalidates_views", false);
    return true;
}

static bool native_parse_return_contract(XrNativePackagePlan *plan, XrTomlValue *table,
                                         XrNativeReturnContract *result, const char *where) {
    static const char *const keys[] = {"ownership", "nullable", "validity", "drop_function"};
    if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
        return false;
    result->ownership = native_ownership(xtoml_get_string(table, "ownership"));
    XrTomlValue *nullable = xtoml_get(table, "nullable");
    result->validity = native_dup_string(table, "validity");
    result->drop_function = native_dup_string(table, "drop_function");
    if (!result->ownership || !nullable || nullable->type != XR_TOML_BOOL || !result->validity ||
        !result->validity[0])
        return native_fail(plan, "E-NATIVE-CONTRACT: %s requires ownership, nullable, and validity",
                           where);
    result->nullable = nullable->as.boolean;
    if (result->ownership == XR_NATIVE_OWNERSHIP_OWNED && !result->drop_function)
        return native_fail(plan, "E-NATIVE-CONTRACT: %s owned return requires drop_function",
                           where);
    return true;
}

static bool native_parse_callback_contracts(XrNativePackagePlan *plan, XrTomlValue *table,
                                            XrNativeSymbolContract *contract, uint32_t param_count,
                                            const char *where) {
    static const char *const keys[] = {
        "index", "context_index", "escape", "thread", "lifetime", "runtime_attach", "reentrant",
    };
    XrTomlValue *array = xtoml_get_array(table, "callbacks");
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return true;
    contract->callbacks =
        (XrNativeCallbackContract *) xr_calloc((size_t) count, sizeof(XrNativeCallbackContract));
    if (!contract->callbacks)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate %s.callbacks", where);
    contract->callback_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        XrTomlValue *entry = xtoml_array_get(array, i);
        XrNativeCallbackContract *callback = &contract->callbacks[i];
        char callback_where[128];
        snprintf(callback_where, sizeof(callback_where), "%s.callbacks[%d]", where, i);
        if (!native_validate_keys(plan, entry, callback_where, keys,
                                  sizeof(keys) / sizeof(keys[0])))
            return false;
        int64_t index = xtoml_get_int_or(entry, "index", -1);
        int64_t context_index = xtoml_get_int_or(entry, "context_index", -1);
        XrTomlValue *reentrant = xtoml_get(entry, "reentrant");
        const char *thread = xtoml_get_string(entry, "thread");
        const char *lifetime = xtoml_get_string(entry, "lifetime");
        const char *attach = xtoml_get_string(entry, "runtime_attach");
        callback->escape = native_escape(xtoml_get_string(entry, "escape"));
        if (index < 0 || (uint64_t) index >= param_count || context_index < -1 ||
            (context_index >= 0 && (uint64_t) context_index >= param_count) || !callback->escape ||
            !reentrant || reentrant->type != XR_TOML_BOOL)
            return native_fail(plan, "E-NATIVE-CONTRACT: %s is incomplete", callback_where);
        callback->index = (uint32_t) index;
        callback->context_index = (int32_t) context_index;
        callback->reentrant = reentrant->as.boolean;
        if (thread && strcmp(thread, "caller") == 0)
            callback->thread = XR_NATIVE_CALLBACK_CALLER_THREAD;
        else if (thread && strcmp(thread, "foreign") == 0)
            callback->thread = XR_NATIVE_CALLBACK_FOREIGN_THREAD;
        else
            return native_fail(plan, "E-NATIVE-CONTRACT: %s.thread must be caller or foreign",
                               callback_where);
        if (lifetime && strcmp(lifetime, "call") == 0)
            callback->lifetime = XR_NATIVE_CALLBACK_CALL_ONLY;
        else if (lifetime && strcmp(lifetime, "retained") == 0)
            callback->lifetime = XR_NATIVE_CALLBACK_RETAINED;
        else
            return native_fail(plan, "E-NATIVE-CONTRACT: %s.lifetime must be call or retained",
                               callback_where);
        if (attach && strcmp(attach, "not-required") == 0)
            callback->runtime_attach = XR_NATIVE_RUNTIME_ATTACH_NOT_REQUIRED;
        else if (attach && strcmp(attach, "attach-detach") == 0)
            callback->runtime_attach = XR_NATIVE_RUNTIME_ATTACH_DETACH;
        else
            return native_fail(
                plan, "E-NATIVE-CONTRACT: %s.runtime_attach must be not-required or attach-detach",
                callback_where);
        if (callback->thread == XR_NATIVE_CALLBACK_FOREIGN_THREAD &&
            callback->runtime_attach != XR_NATIVE_RUNTIME_ATTACH_DETACH)
            return native_fail(plan,
                               "E-NATIVE-CONTRACT: foreign-thread callback %u requires an "
                               "attach-detach runtime plan",
                               callback->index);
        if (callback->lifetime == XR_NATIVE_CALLBACK_RETAINED &&
            callback->escape != XR_NATIVE_ESCAPE_RETAIN)
            return native_fail(plan,
                               "E-NATIVE-CONTRACT: retained callback %u requires escape=retain",
                               callback->index);
    }
    return true;
}

static bool native_parse_symbol_contract(XrNativePackagePlan *plan, XrTomlValue *table,
                                         XrNativeSymbolContract *contract, const char *where) {
    static const char *const keys[] = {
        "params",   "return",  "effects", "callbacks", "failure", "allocation",
        "blocking", "suspend", "io",      "sync",      "panic",   "error",
    };
    if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
        return false;
    XrTomlValue *params = xtoml_get_array(table, "params");
    XrTomlValue *result = xtoml_get_table(table, "return");
    if (!params || !result)
        return native_fail(plan, "E-NATIVE-CONTRACT: %s requires params and return", where);
    int count = xtoml_array_len(params);
    if (count > 0) {
        contract->params =
            (XrNativeParamContract *) xr_calloc((size_t) count, sizeof(XrNativeParamContract));
        if (!contract->params)
            return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate %s.params", where);
        contract->param_count = (uint32_t) count;
    }
    for (int i = 0; i < count; i++) {
        char param_where[112];
        snprintf(param_where, sizeof(param_where), "%s.params[%d]", where, i);
        if (!native_parse_param_contract(plan, xtoml_array_get(params, i), &contract->params[i],
                                         (uint32_t) i, param_where))
            return false;
    }
    for (int i = 0; i < count; i++) {
        XrNativeParamContract *param = &contract->params[i];
        if (param->length_from >= count || param->length_from == i)
            return native_fail(plan,
                               "E-NATIVE-CONTRACT: %s.params[%d].length_from must name another "
                               "parameter or be omitted",
                               where, i);
        if (param->output != XR_NATIVE_OUTPUT_NONE && param->access != XR_NATIVE_ACCESS_WRITE &&
            param->access != XR_NATIVE_ACCESS_READWRITE)
            return native_fail(plan,
                               "E-NATIVE-CONTRACT: %s.params[%d] output requires write or "
                               "readwrite access",
                               where, i);
        if (param->output == XR_NATIVE_OUTPUT_PARTIAL &&
            plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING)
            return native_fail(plan,
                               "E-NATIVE-CONTRACT: shipping output parameter %d is partial; "
                               "typed materialization requires complete validity evidence",
                               i);
    }
    char return_where[112];
    snprintf(return_where, sizeof(return_where), "%s.return", where);
    if (!native_parse_return_contract(plan, result, &contract->result, return_where))
        return false;
    if (!native_parse_string_array(plan, table, "effects", true, &contract->effects,
                                   &contract->effect_count, where) ||
        !native_parse_callback_contracts(plan, table, contract, (uint32_t) count, where))
        return false;
    static const char *const allowed_effects[] = {
        "foreign", "alloc", "may_block", "suspend", "io", "sync", "panic", "abort",
    };
    for (uint32_t i = 0; i < contract->effect_count; i++) {
        if (!native_text_is(contract->effects[i], allowed_effects,
                            sizeof(allowed_effects) / sizeof(allowed_effects[0])))
            return native_fail(plan, "E-NATIVE-CONTRACT: %s.effects[%u] is not a sealed effect",
                               where, i);
    }
    contract->failure = native_dup_string(table, "failure");
    contract->allocation = native_dup_string(table, "allocation");
    contract->blocking = native_dup_string(table, "blocking");
    contract->suspend = native_dup_string(table, "suspend");
    contract->io = native_dup_string(table, "io");
    contract->sync = native_dup_string(table, "sync");
    contract->panic = native_dup_string(table, "panic");
    contract->error = native_dup_string(table, "error");
    if (!contract->failure || !contract->allocation || !contract->blocking || !contract->suspend ||
        !contract->io || !contract->sync || !contract->panic || !contract->error)
        return native_fail(plan,
                           "E-NATIVE-CONTRACT: %s requires failure/allocation/blocking/suspend/"
                           "io/sync/panic/error",
                           where);
    static const char *const failure_values[] = {"none", "status_nonzero", "null", "errno"};
    static const char *const allocation_values[] = {"none", "may"};
    static const char *const binary_values[] = {"never", "may"};
    static const char *const io_values[] = {"none", "read", "write", "readwrite"};
    static const char *const sync_values[] = {"none", "internal", "external"};
    static const char *const panic_values[] = {"never", "abort"};
    static const char *const error_values[] = {"none", "status", "errno", "result"};
    if (!native_text_is(contract->failure, failure_values,
                        sizeof(failure_values) / sizeof(failure_values[0])) ||
        !native_text_is(contract->allocation, allocation_values,
                        sizeof(allocation_values) / sizeof(allocation_values[0])) ||
        !native_text_is(contract->blocking, binary_values,
                        sizeof(binary_values) / sizeof(binary_values[0])) ||
        !native_text_is(contract->suspend, binary_values,
                        sizeof(binary_values) / sizeof(binary_values[0])) ||
        !native_text_is(contract->io, io_values, sizeof(io_values) / sizeof(io_values[0])) ||
        !native_text_is(contract->sync, sync_values,
                        sizeof(sync_values) / sizeof(sync_values[0])) ||
        !native_text_is(contract->panic, panic_values,
                        sizeof(panic_values) / sizeof(panic_values[0])) ||
        !native_text_is(contract->error, error_values,
                        sizeof(error_values) / sizeof(error_values[0])))
        return native_fail(plan, "E-NATIVE-CONTRACT: %s contains an unknown typed contract value",
                           where);
    contract->complete = true;
    return true;
}

static bool native_parse_symbols(XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const symbol_keys[] = {"xray", "native",  "kind", "calling_convention",
                                              "unit", "contract"};
    XrTomlValue *array = xtoml_get_array(native, "symbol");
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(plan, "E-NATIVE-SCHEMA: native.symbol cannot be empty");
    plan->symbols = (XrNativeSymbol *) xr_calloc((size_t) count, sizeof(XrNativeSymbol));
    if (!plan->symbols)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate native symbols");
    plan->symbol_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        char where[64];
        snprintf(where, sizeof(where), "native.symbol[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeSymbol *symbol = &plan->symbols[i];
        if (!native_validate_keys(plan, table, where, symbol_keys,
                                  sizeof(symbol_keys) / sizeof(symbol_keys[0])))
            return false;
        symbol->xray_name = native_dup_string(table, "xray");
        symbol->native_name = native_dup_string(table, "native");
        symbol->kind = native_symbol_kind(xtoml_get_string(table, "kind"));
        symbol->calling_convention = native_dup_string(table, "calling_convention");
        symbol->unit_name = native_dup_string(table, "unit");
        if (!symbol->xray_name || !symbol->native_name || !symbol->kind ||
            !symbol->calling_convention || strcmp(symbol->calling_convention, "c") != 0 ||
            !symbol->unit_name)
            return native_fail(plan,
                               "E-NATIVE-SCHEMA: %s requires xray/native/kind/c calling "
                               "convention/unit",
                               where);
        symbol->unit = xr_native_package_find_unit(plan, symbol->unit_name);
        if (!symbol->unit)
            return native_fail(plan, "E-NATIVE-SCHEMA: %s references unknown unit '%s'", where,
                               symbol->unit_name);
        for (int j = 0; j < i; j++) {
            if (strcmp(plan->symbols[j].xray_name, symbol->xray_name) == 0)
                return native_fail(plan, "E-NATIVE-SCHEMA: duplicate Xray symbol '%s'",
                                   symbol->xray_name);
        }
        XrTomlValue *contract = xtoml_get_table(table, "contract");
        if (!contract) {
            if (plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING)
                return native_fail(plan, "E-NATIVE-CONTRACT: shipping symbol '%s' has no contract",
                                   symbol->xray_name);
            continue;
        }
        char contract_where[80];
        snprintf(contract_where, sizeof(contract_where), "%s.contract", where);
        if (!native_parse_symbol_contract(plan, contract, &symbol->contract, contract_where))
            return false;
    }
    return true;
}

static bool native_parse_layouts(XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const layout_keys[] = {"xray_type", "c_type", "header", "assert"};
    static const char *const assert_keys[] = {"size", "align", "fields"};
    XrTomlValue *array = xtoml_get_array(native, "layout");
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(plan, "E-NATIVE-SCHEMA: native.layout cannot be empty");
    plan->layouts =
        (XrNativeLayoutAssertion *) xr_calloc((size_t) count, sizeof(XrNativeLayoutAssertion));
    if (!plan->layouts)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate native layouts");
    plan->layout_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        char where[64];
        snprintf(where, sizeof(where), "native.layout[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeLayoutAssertion *layout = &plan->layouts[i];
        if (!native_validate_keys(plan, table, where, layout_keys,
                                  sizeof(layout_keys) / sizeof(layout_keys[0])))
            return false;
        layout->xray_type = native_dup_string(table, "xray_type");
        layout->c_type = native_dup_string(table, "c_type");
        const char *header = xtoml_get_string(table, "header");
        if (!layout->xray_type || !native_valid_c_identifier(layout->c_type) || !header)
            return native_fail(plan, "E-NATIVE-LAYOUT: %s requires xray_type/c_type/header", where);
        char header_where[80];
        snprintf(header_where, sizeof(header_where), "%s.header", where);
        layout->header = native_resolve_existing_path(plan, header, header_where);
        if (!layout->header)
            return false;
        XrTomlValue *assertions = xtoml_get_table(table, "assert");
        if (!native_validate_keys(plan, assertions, "native.layout.assert", assert_keys,
                                  sizeof(assert_keys) / sizeof(assert_keys[0])))
            return false;
        layout->assert_size = xtoml_get_bool_or(assertions, "size", false);
        layout->assert_align = xtoml_get_bool_or(assertions, "align", false);
        layout->assert_fields = xtoml_get_bool_or(assertions, "fields", false);
        if (!layout->assert_size || !layout->assert_align || !layout->assert_fields)
            return native_fail(plan, "E-NATIVE-LAYOUT: %s must assert size, align, and fields",
                               where);
    }
    return true;
}

static bool native_parse_capabilities(XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const keys[] = {"type", "request", "attestation", "scope"};
    XrTomlValue *array = xtoml_get_array(native, "capability");
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(plan, "E-NATIVE-SCHEMA: native.capability cannot be empty");
    plan->capabilities =
        (XrNativeCapability *) xr_calloc((size_t) count, sizeof(XrNativeCapability));
    if (!plan->capabilities)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate native capabilities");
    plan->capability_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        char where[72];
        snprintf(where, sizeof(where), "native.capability[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrNativeCapability *cap = &plan->capabilities[i];
        if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        cap->type_name = native_dup_string(table, "type");
        cap->request = native_dup_string(table, "request");
        cap->attestation = native_dup_string(table, "attestation");
        cap->scope = native_dup_string(table, "scope");
        if (!cap->type_name || !cap->request || !cap->attestation || !cap->scope)
            return native_fail(plan, "E-NATIVE-CAPABILITY: %s is incomplete", where);
        /* The package manifest is not an authority.  No external trust registry
         * has been installed in this compiler build, so every requested native
         * capability remains unverified and shipping builds fail closed. */
        cap->verified = false;
        if (plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING)
            return native_fail(plan,
                               "E-FFI-UNPROVEN-SHARE: capability for '%s' has no compiler/SDK "
                               "trust-registry attestation",
                               cap->type_name);
    }
    return true;
}

static bool native_valid_target_triple(const char *triple) {
    if (!triple || !triple[0])
        return false;
    for (const unsigned char *p = (const unsigned char *) triple; *p; p++) {
        if (!isalnum(*p) && *p != '-' && *p != '_' && *p != '.')
            return false;
    }
    return strstr(triple, "..") == NULL;
}

static bool native_valid_c_identifier(const char *name) {
    if (!name || !name[0] || !(isalpha((unsigned char) name[0]) || name[0] == '_'))
        return false;
    for (const unsigned char *p = (const unsigned char *) name + 1; *p; p++) {
        if (!isalnum(*p) && *p != '_')
            return false;
    }
    return true;
}

static bool native_parse_targets(XrNativePackagePlan *plan, XrTomlValue *native) {
    static const char *const keys[] = {
        "profile", "visibility", "cpu_feature", "system_links", "vm",
    };
    XrTomlValue *targets = xtoml_get_table(native, "target");
    if (!targets)
        return true;
    if (targets->as.table.count <= 0)
        return native_fail(plan, "E-NATIVE-SCHEMA: native.target cannot be empty");
    plan->targets = (XrNativeTargetPlan *) xr_calloc((size_t) targets->as.table.count,
                                                     sizeof(XrNativeTargetPlan));
    if (!plan->targets)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate native target plans");
    plan->target_count = (uint32_t) targets->as.table.count;
    for (int i = 0; i < targets->as.table.count; i++) {
        const char *triple = targets->as.table.members[i].key;
        XrTomlValue *table = targets->as.table.members[i].value;
        XrNativeTargetPlan *target = &plan->targets[i];
        char where[192];
        snprintf(where, sizeof(where), "native.target.%s", triple ? triple : "?");
        if (!native_valid_target_triple(triple))
            return native_fail(plan, "E-NATIVE-TARGET: invalid canonical target triple '%s'",
                               triple ? triple : "");
        if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        target->triple = xr_strdup(triple);
        target->profile = native_dup_string(table, "profile");
        target->visibility = native_dup_string(table, "visibility");
        target->cpu_feature = native_dup_string(table, "cpu_feature");
        if (!target->triple || !target->profile ||
            (strcmp(target->profile, "debug") != 0 && strcmp(target->profile, "release") != 0 &&
             strcmp(target->profile, "freestanding") != 0))
            return native_fail(plan,
                               "E-NATIVE-TARGET: %s.profile must be debug, release, or "
                               "freestanding",
                               where);
        if (target->visibility && strcmp(target->visibility, "hidden") != 0 &&
            strcmp(target->visibility, "default") != 0)
            return native_fail(plan, "E-NATIVE-TARGET: %s.visibility must be hidden or default",
                               where);
        if (target->cpu_feature && strcmp(target->cpu_feature, "baseline") != 0)
            return native_fail(
                plan, "E-NATIVE-TARGET: %s.cpu_feature must be the sealed value baseline", where);
        if (!native_parse_string_array(plan, table, "system_links", false, &target->system_links,
                                       &target->system_link_count, where))
            return false;
        const char *vm = xtoml_get_string(table, "vm");
        if (vm) {
            if (strcmp(vm, "verified-dynamic") == 0)
                target->vm_policy = XR_NATIVE_VM_VERIFIED_DYNAMIC;
            else if (strcmp(vm, "unsupported") == 0)
                target->vm_policy = XR_NATIVE_VM_UNSUPPORTED;
            else
                return native_fail(plan,
                                   "E-NATIVE-TARGET: %s.vm must be verified-dynamic or "
                                   "unsupported",
                                   where);
        } else {
            target->vm_policy = plan->vm_policy;
        }
    }
    return true;
}

static bool native_parse_exports(XrNativePackagePlan *plan, XrTomlValue *root) {
    static const char *const keys[] = {"xray", "symbol", "visibility", "header"};
    XrTomlValue *group = xtoml_get_table(root, "export");
    XrTomlValue *array = group ? xtoml_get_array(group, "c") : NULL;
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(plan, "E-EXPORT-SCHEMA: export.c cannot be empty");
    plan->exports = (XrCExportPlan *) xr_calloc((size_t) count, sizeof(XrCExportPlan));
    if (!plan->exports)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate C export plans");
    plan->export_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        char where[64];
        snprintf(where, sizeof(where), "export.c[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrCExportPlan *item = &plan->exports[i];
        if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        item->xray_name = native_dup_string(table, "xray");
        item->symbol = native_dup_string(table, "symbol");
        item->visibility = native_dup_string(table, "visibility");
        item->header = xtoml_get_bool_or(table, "header", false);
        if (!item->xray_name || !item->xray_name[0] || !native_valid_c_identifier(item->symbol))
            return native_fail(plan, "E-EXPORT-SCHEMA: %s requires xray and C identifier symbol",
                               where);
        if (item->visibility && strcmp(item->visibility, "default") != 0 &&
            strcmp(item->visibility, "hidden") != 0)
            return native_fail(plan, "E-EXPORT-SCHEMA: %s.visibility must be default or hidden",
                               where);
        for (int j = 0; j < i; j++) {
            if (strcmp(plan->exports[j].xray_name, item->xray_name) == 0 ||
                strcmp(plan->exports[j].symbol, item->symbol) == 0)
                return native_fail(plan, "E-EXPORT-SCHEMA: duplicate export '%s'", item->xray_name);
        }
    }
    return true;
}

static bool native_parse_link_symbols(XrNativePackagePlan *plan, XrTomlValue *root) {
    static const char *const keys[] = {"xray", "section", "used", "weak"};
    XrTomlValue *group = xtoml_get_table(root, "link");
    XrTomlValue *array = group ? xtoml_get_array(group, "symbol") : NULL;
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(plan, "E-LINK-SCHEMA: link.symbol cannot be empty");
    plan->link_symbols = (XrLinkSymbolPlan *) xr_calloc((size_t) count, sizeof(XrLinkSymbolPlan));
    if (!plan->link_symbols)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate link symbol plans");
    plan->link_symbol_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        char where[64];
        snprintf(where, sizeof(where), "link.symbol[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrLinkSymbolPlan *item = &plan->link_symbols[i];
        if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        item->xray_name = native_dup_string(table, "xray");
        item->section = native_dup_string(table, "section");
        item->used = xtoml_get_bool_or(table, "used", false);
        item->weak = xtoml_get_bool_or(table, "weak", false);
        if (!item->xray_name || !item->xray_name[0])
            return native_fail(plan, "E-LINK-SCHEMA: %s requires non-empty xray", where);
        if (item->section && !item->section[0])
            return native_fail(plan, "E-LINK-SCHEMA: %s.section cannot be empty", where);
        if (!item->section && !item->used && !item->weak)
            return native_fail(plan, "E-LINK-SCHEMA: %s requires section, used=true, or weak=true",
                               where);
        for (int j = 0; j < i; j++)
            if (strcmp(plan->link_symbols[j].xray_name, item->xray_name) == 0)
                return native_fail(plan, "E-LINK-SCHEMA: duplicate link symbol '%s'",
                                   item->xray_name);
    }
    return true;
}

static XrFreestandingEntryKind native_entry_kind(const char *kind) {
    if (!kind)
        return 0;
    if (strcmp(kind, "start") == 0)
        return XR_FREESTANDING_ENTRY_START;
    if (strcmp(kind, "interrupt") == 0)
        return XR_FREESTANDING_ENTRY_INTERRUPT;
    if (strcmp(kind, "naked-stub") == 0)
        return XR_FREESTANDING_ENTRY_NAKED_STUB;
    return 0;
}

static bool native_parse_entries(XrNativePackagePlan *plan, XrTomlValue *root) {
    static const char *const keys[] = {"xray", "symbol", "kind", "abi", "section", "stub"};
    XrTomlValue *group = xtoml_get_table(root, "freestanding");
    XrTomlValue *array = group ? xtoml_get_array(group, "entry") : NULL;
    if (!array)
        return true;
    int count = xtoml_array_len(array);
    if (count <= 0)
        return native_fail(plan, "E-ENTRY-SCHEMA: freestanding.entry cannot be empty");
    plan->entries =
        (XrFreestandingEntryPlan *) xr_calloc((size_t) count, sizeof(XrFreestandingEntryPlan));
    if (!plan->entries)
        return native_fail(plan, "E-NATIVE-RESOURCE: cannot allocate freestanding entry plans");
    plan->entry_count = (uint32_t) count;
    for (int i = 0; i < count; i++) {
        char where[72];
        snprintf(where, sizeof(where), "freestanding.entry[%d]", i);
        XrTomlValue *table = xtoml_array_get(array, i);
        XrFreestandingEntryPlan *item = &plan->entries[i];
        if (!native_validate_keys(plan, table, where, keys, sizeof(keys) / sizeof(keys[0])))
            return false;
        item->xray_name = native_dup_string(table, "xray");
        item->symbol = native_dup_string(table, "symbol");
        item->kind = native_entry_kind(xtoml_get_string(table, "kind"));
        item->abi = native_dup_string(table, "abi");
        item->section = native_dup_string(table, "section");
        const char *stub = xtoml_get_string(table, "stub");
        if (stub) {
            char stub_where[96];
            snprintf(stub_where, sizeof(stub_where), "%s.stub", where);
            item->stub = native_resolve_existing_path(plan, stub, stub_where);
            if (!item->stub)
                return false;
        }
        if (!item->xray_name || !item->xray_name[0] || !native_valid_c_identifier(item->symbol) ||
            !item->kind || !item->section || !item->section[0])
            return native_fail(plan, "E-ENTRY-SCHEMA: %s requires xray/symbol/kind/section", where);
        if (item->kind == XR_FREESTANDING_ENTRY_INTERRUPT && (!item->abi || !item->abi[0]))
            return native_fail(plan, "E-ENTRY-SCHEMA: %s interrupt requires abi", where);
        if ((item->kind == XR_FREESTANDING_ENTRY_NAKED_STUB ||
             item->kind == XR_FREESTANDING_ENTRY_INTERRUPT) &&
            !item->stub)
            return native_fail(plan, "E-ENTRY-SCHEMA: %s requires an audited stub", where);
        for (int j = 0; j < i; j++)
            if (strcmp(plan->entries[j].xray_name, item->xray_name) == 0 ||
                strcmp(plan->entries[j].symbol, item->symbol) == 0)
                return native_fail(plan, "E-ENTRY-SCHEMA: duplicate entry '%s'", item->xray_name);
    }
    return true;
}

static void native_refresh_plan_fingerprint(XrNativePackagePlan *plan) {
    uint64_t fingerprint = XR_NATIVE_FNV_OFFSET;
    fingerprint = native_hash_text(fingerprint, plan->name);
    fingerprint = native_hash_text(fingerprint, plan->version);
    fingerprint = native_hash_text(fingerprint, plan->license);
    fingerprint = native_hash_text(fingerprint, plan->source);
    fingerprint = native_hash_bytes(fingerprint, &plan->audit_mode, sizeof(plan->audit_mode));
    fingerprint = native_hash_bytes(fingerprint, &plan->vm_policy, sizeof(plan->vm_policy));
    for (uint32_t i = 0; i < plan->unit_count; i++) {
        fingerprint = native_hash_text(fingerprint, plan->units[i].name);
        fingerprint = native_hash_bytes(fingerprint, &plan->units[i].fingerprint,
                                        sizeof(plan->units[i].fingerprint));
    }
    for (uint32_t i = 0; i < plan->symbol_count; i++) {
        fingerprint = native_hash_text(fingerprint, plan->symbols[i].xray_name);
        fingerprint = native_hash_text(fingerprint, plan->symbols[i].native_name);
    }
    for (uint32_t i = 0; i < plan->target_count; i++) {
        fingerprint = native_hash_text(fingerprint, plan->targets[i].triple);
        fingerprint = native_hash_text(fingerprint, plan->targets[i].profile);
        fingerprint = native_hash_text(fingerprint, plan->targets[i].visibility);
        fingerprint = native_hash_text(fingerprint, plan->targets[i].cpu_feature);
        for (uint32_t j = 0; j < plan->targets[i].system_link_count; j++)
            fingerprint = native_hash_text(fingerprint, plan->targets[i].system_links[j]);
        fingerprint = native_hash_bytes(fingerprint, &plan->targets[i].vm_policy,
                                        sizeof(plan->targets[i].vm_policy));
    }
    for (uint32_t i = 0; i < plan->export_count; i++) {
        fingerprint = native_hash_text(fingerprint, plan->exports[i].xray_name);
        fingerprint = native_hash_text(fingerprint, plan->exports[i].symbol);
        fingerprint = native_hash_text(fingerprint, plan->exports[i].visibility);
        fingerprint = native_hash_bytes(fingerprint, &plan->exports[i].header,
                                        sizeof(plan->exports[i].header));
    }
    for (uint32_t i = 0; i < plan->link_symbol_count; i++) {
        fingerprint = native_hash_text(fingerprint, plan->link_symbols[i].xray_name);
        fingerprint = native_hash_text(fingerprint, plan->link_symbols[i].section);
        fingerprint = native_hash_bytes(fingerprint, &plan->link_symbols[i].used,
                                        sizeof(plan->link_symbols[i].used));
        fingerprint = native_hash_bytes(fingerprint, &plan->link_symbols[i].weak,
                                        sizeof(plan->link_symbols[i].weak));
    }
    for (uint32_t i = 0; i < plan->entry_count; i++) {
        fingerprint = native_hash_text(fingerprint, plan->entries[i].xray_name);
        fingerprint = native_hash_text(fingerprint, plan->entries[i].symbol);
        fingerprint =
            native_hash_bytes(fingerprint, &plan->entries[i].kind, sizeof(plan->entries[i].kind));
        fingerprint = native_hash_text(fingerprint, plan->entries[i].abi);
        fingerprint = native_hash_text(fingerprint, plan->entries[i].section);
        fingerprint = native_hash_text(fingerprint, plan->entries[i].stub);
    }
    plan->fingerprint = fingerprint ? fingerprint : 1;
}

XrNativePackagePlan *xr_native_package_plan_parse(XrTomlValue *toml_root,
                                                  const char *project_root) {
    static const char *const native_keys[] = {
        "name", "version", "license", "source",     "audit_mode", "vm",
        "unit", "symbol",  "layout",  "capability", "target",
    };
    if (!toml_root || !project_root)
        return NULL;
    XrTomlValue *native = xtoml_get_table(toml_root, "native");
    bool has_export = xtoml_get_table(toml_root, "export") != NULL;
    bool has_link = xtoml_get_table(toml_root, "link") != NULL;
    bool has_entry = xtoml_get_table(toml_root, "freestanding") != NULL;
    if (!native && !has_export && !has_link && !has_entry)
        return NULL;
    XrNativePackagePlan *plan = (XrNativePackagePlan *) xr_calloc(1, sizeof(XrNativePackagePlan));
    if (!plan)
        return NULL;
    plan->root = xr_realpath(project_root);
    if (!plan->root)
        plan->root = xr_strdup(project_root);
    plan->valid = true;
    if (native) {
        if (!native_validate_keys(plan, native, "native", native_keys,
                                  sizeof(native_keys) / sizeof(native_keys[0])))
            return plan;
        plan->name = native_dup_string(native, "name");
        plan->version = native_dup_string(native, "version");
        plan->license = native_dup_string(native, "license");
        plan->source = native_dup_string(native, "source");
        if (!plan->name || !plan->version || !plan->license || !plan->source ||
            !native_parse_audit(plan, xtoml_get_string(native, "audit_mode")) ||
            !native_parse_vm_policy(plan, xtoml_get_string(native, "vm"))) {
            if (!plan->error)
                native_fail(plan, "E-NATIVE-SCHEMA: native requires "
                                  "name/version/license/source/audit_mode/vm");
            return plan;
        }
        if (!native_parse_units(plan, native) || !native_parse_symbols(plan, native) ||
            !native_parse_layouts(plan, native) || !native_parse_capabilities(plan, native) ||
            !native_parse_targets(plan, native))
            return plan;
    } else {
        plan->name = xr_strdup("project");
        plan->version = xr_strdup("0");
        plan->license = xr_strdup("project");
        plan->source = xr_strdup("project");
        plan->audit_mode = XR_NATIVE_AUDIT_NONE;
        plan->vm_policy = XR_NATIVE_VM_UNSUPPORTED;
    }
    if (!native_parse_exports(plan, toml_root) || !native_parse_link_symbols(plan, toml_root) ||
        !native_parse_entries(plan, toml_root))
        return plan;
    if (plan->audit_mode == XR_NATIVE_AUDIT_SHIPPING &&
        (plan->unit_count == 0 || (plan->symbol_count == 0 && plan->entry_count == 0))) {
        native_fail(plan, "E-NATIVE-SCHEMA: shipping native package requires units and symbols or "
                          "freestanding entries");
        return plan;
    }
    native_refresh_plan_fingerprint(plan);
    return plan;
}

static void native_symbol_contract_free(XrNativeSymbolContract *contract) {
    if (!contract)
        return;
    xr_free(contract->params);
    xr_free(contract->result.validity);
    xr_free(contract->result.drop_function);
    native_free_string_array(contract->effects, contract->effect_count);
    xr_free(contract->callbacks);
    xr_free(contract->failure);
    xr_free(contract->allocation);
    xr_free(contract->blocking);
    xr_free(contract->suspend);
    xr_free(contract->io);
    xr_free(contract->sync);
    xr_free(contract->panic);
    xr_free(contract->error);
}

void xr_native_package_plan_free(XrNativePackagePlan *plan) {
    if (!plan)
        return;
    xr_free(plan->root);
    xr_free(plan->name);
    xr_free(plan->version);
    xr_free(plan->license);
    xr_free(plan->source);
    xr_free(plan->error);
    for (uint32_t i = 0; i < plan->unit_count; i++) {
        XrNativeUnit *unit = &plan->units[i];
        xr_free(unit->name);
        native_free_string_array(unit->sources, unit->source_count);
        native_free_string_array(unit->source_relpaths, unit->source_count);
        native_free_string_array(unit->source_hashes, unit->source_count);
        native_free_string_array(unit->include_dirs, unit->include_dir_count);
        native_free_string_array(unit->defines, unit->define_count);
        native_free_string_array(unit->system_links, unit->system_link_count);
        xr_free(unit->language_standard);
        xr_free(unit->optimization);
        xr_free(unit->visibility);
        xr_free(unit->warning_policy);
        xr_free(unit->cpu_feature);
        xr_free(unit->output);
        xr_free(unit->purpose);
    }
    xr_free(plan->units);
    for (uint32_t i = 0; i < plan->symbol_count; i++) {
        XrNativeSymbol *symbol = &plan->symbols[i];
        xr_free(symbol->xray_name);
        xr_free(symbol->native_name);
        xr_free(symbol->calling_convention);
        xr_free(symbol->unit_name);
        native_symbol_contract_free(&symbol->contract);
    }
    xr_free(plan->symbols);
    for (uint32_t i = 0; i < plan->layout_count; i++) {
        xr_free(plan->layouts[i].xray_type);
        xr_free(plan->layouts[i].c_type);
        xr_free(plan->layouts[i].header);
        native_free_string_array(plan->layouts[i].field_names, plan->layouts[i].field_count);
        xr_free(plan->layouts[i].field_offsets);
    }
    xr_free(plan->layouts);
    for (uint32_t i = 0; i < plan->capability_count; i++) {
        xr_free(plan->capabilities[i].type_name);
        xr_free(plan->capabilities[i].request);
        xr_free(plan->capabilities[i].attestation);
        xr_free(plan->capabilities[i].scope);
    }
    xr_free(plan->capabilities);
    for (uint32_t i = 0; i < plan->target_count; i++) {
        xr_free(plan->targets[i].triple);
        xr_free(plan->targets[i].profile);
        xr_free(plan->targets[i].visibility);
        xr_free(plan->targets[i].cpu_feature);
        native_free_string_array(plan->targets[i].system_links, plan->targets[i].system_link_count);
    }
    xr_free(plan->targets);
    for (uint32_t i = 0; i < plan->export_count; i++) {
        xr_free(plan->exports[i].xray_name);
        xr_free(plan->exports[i].symbol);
        xr_free(plan->exports[i].visibility);
    }
    xr_free(plan->exports);
    for (uint32_t i = 0; i < plan->link_symbol_count; i++) {
        xr_free(plan->link_symbols[i].xray_name);
        xr_free(plan->link_symbols[i].section);
    }
    xr_free(plan->link_symbols);
    for (uint32_t i = 0; i < plan->entry_count; i++) {
        xr_free(plan->entries[i].xray_name);
        xr_free(plan->entries[i].symbol);
        xr_free(plan->entries[i].abi);
        xr_free(plan->entries[i].section);
        xr_free(plan->entries[i].stub);
    }
    xr_free(plan->entries);
    xr_free(plan);
}

const XrNativeUnit *xr_native_package_find_unit(const XrNativePackagePlan *plan, const char *name) {
    if (!plan || !name)
        return NULL;
    for (uint32_t i = 0; i < plan->unit_count; i++) {
        if (plan->units[i].name && strcmp(plan->units[i].name, name) == 0)
            return &plan->units[i];
    }
    return NULL;
}

static const char *native_final_component(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot ? dot + 1 : name;
}

const XrNativeSymbol *xr_native_package_find_symbol(const XrNativePackagePlan *plan,
                                                    const char *xray_name) {
    const XrNativeSymbol *short_match = NULL;
    if (!plan || !plan->valid || !xray_name)
        return NULL;
    for (uint32_t i = 0; i < plan->symbol_count; i++) {
        const XrNativeSymbol *symbol = &plan->symbols[i];
        if (strcmp(symbol->xray_name, xray_name) == 0)
            return symbol;
        if (strcmp(native_final_component(symbol->xray_name), xray_name) == 0) {
            if (short_match)
                return NULL;
            short_match = symbol;
        }
    }
    return short_match;
}

static bool native_plan_name_matches(const char *declared, const char *requested) {
    if (!declared || !requested)
        return false;
    if (strcmp(declared, requested) == 0)
        return true;
    const char *dot = strrchr(declared, '.');
    return dot && strcmp(dot + 1, requested) == 0;
}

const XrCExportPlan *xr_native_package_find_export(const XrNativePackagePlan *plan,
                                                   const char *xray_name) {
    const XrCExportPlan *found = NULL;
    if (!plan || !xray_name)
        return NULL;
    for (uint32_t i = 0; i < plan->export_count; i++) {
        if (!native_plan_name_matches(plan->exports[i].xray_name, xray_name))
            continue;
        if (found)
            return NULL;
        found = &plan->exports[i];
    }
    return found;
}

static bool native_csv_has_symbol(const char *csv, const char *symbol) {
    if (!csv || !csv[0] || !symbol)
        return false;
    size_t symbol_len = strlen(symbol);
    const char *item = csv;
    while (*item) {
        const char *comma = strchr(item, ',');
        size_t item_len = comma ? (size_t) (comma - item) : strlen(item);
        if (item_len == symbol_len && memcmp(item, symbol, item_len) == 0)
            return true;
        if (!comma)
            break;
        item = comma + 1;
    }
    return false;
}

static bool native_validate_export_excludes(const XrNativePackagePlan *plan, const char *csv,
                                            char *error, size_t error_size) {
    if (!csv || !csv[0])
        return true;
    const char *item = csv;
    while (true) {
        const char *comma = strchr(item, ',');
        size_t item_len = comma ? (size_t) (comma - item) : strlen(item);
        if (item_len == 0) {
            if (error && error_size)
                snprintf(error, error_size, "--c-export-exclude contains an empty symbol");
            return false;
        }
        char *symbol = (char *) xr_malloc(item_len + 1);
        if (!symbol) {
            if (error && error_size)
                snprintf(error, error_size, "cannot allocate C export filter");
            return false;
        }
        memcpy(symbol, item, item_len);
        symbol[item_len] = '\0';
        bool valid = native_valid_c_identifier(symbol);
        bool found = false;
        if (valid) {
            for (uint32_t i = 0; i < plan->export_count; i++) {
                if (strcmp(plan->exports[i].symbol, symbol) == 0) {
                    found = true;
                    break;
                }
            }
        }
        if (!valid && error && error_size)
            snprintf(error, error_size, "invalid C export symbol in --c-export-exclude: %s",
                     symbol);
        else if (!found && error && error_size)
            snprintf(error, error_size, "unknown C export symbol in --c-export-exclude: %s",
                     symbol);
        xr_free(symbol);
        if (!valid || !found)
            return false;
        if (!comma)
            break;
        item = comma + 1;
    }
    return true;
}

bool xr_native_package_configure_c_exports(XrNativePackagePlan *plan, const char *public_prefix,
                                           const char *exclude_csv, char *error,
                                           size_t error_size) {
    if (error && error_size)
        error[0] = '\0';
    if (!plan || !plan->valid) {
        if (error && error_size)
            snprintf(error, error_size, "C export shaping requires a valid project manifest");
        return false;
    }
    if (public_prefix && public_prefix[0] && !native_valid_c_identifier(public_prefix)) {
        if (error && error_size)
            snprintf(error, error_size, "invalid --c-export-prefix C identifier: %s",
                     public_prefix);
        return false;
    }
    if (!native_validate_export_excludes(plan, exclude_csv, error, error_size))
        return false;

    char **renamed = NULL;
    if (public_prefix && public_prefix[0] && plan->export_count > 0) {
        renamed = (char **) xr_calloc(plan->export_count, sizeof(char *));
        if (!renamed) {
            if (error && error_size)
                snprintf(error, error_size, "cannot allocate prefixed C exports");
            return false;
        }
        size_t prefix_len = strlen(public_prefix);
        for (uint32_t i = 0; i < plan->export_count; i++) {
            XrCExportPlan *item = &plan->exports[i];
            if (item->visibility && strcmp(item->visibility, "hidden") == 0)
                continue;
            size_t symbol_len = strlen(item->symbol);
            renamed[i] = (char *) xr_malloc(prefix_len + symbol_len + 1);
            if (!renamed[i]) {
                for (uint32_t j = 0; j < i; j++)
                    xr_free(renamed[j]);
                xr_free(renamed);
                if (error && error_size)
                    snprintf(error, error_size, "cannot allocate prefixed C export symbol");
                return false;
            }
            memcpy(renamed[i], public_prefix, prefix_len);
            memcpy(renamed[i] + prefix_len, item->symbol, symbol_len + 1);
        }
    }

    uint32_t write = 0;
    for (uint32_t i = 0; i < plan->export_count; i++) {
        XrCExportPlan *item = &plan->exports[i];
        if (native_csv_has_symbol(exclude_csv, item->symbol)) {
            xr_free(item->xray_name);
            xr_free(item->symbol);
            xr_free(item->visibility);
            if (renamed)
                xr_free(renamed[i]);
            continue;
        }
        if (renamed && renamed[i]) {
            xr_free(item->symbol);
            item->symbol = renamed[i];
        }
        if (write != i)
            plan->exports[write] = *item;
        write++;
    }
    xr_free(renamed);
    plan->export_count = write;
    native_refresh_plan_fingerprint(plan);
    return true;
}

const XrLinkSymbolPlan *xr_native_package_find_link_symbol(const XrNativePackagePlan *plan,
                                                           const char *xray_name) {
    const XrLinkSymbolPlan *found = NULL;
    if (!plan || !xray_name)
        return NULL;
    for (uint32_t i = 0; i < plan->link_symbol_count; i++) {
        if (!native_plan_name_matches(plan->link_symbols[i].xray_name, xray_name))
            continue;
        if (found)
            return NULL;
        found = &plan->link_symbols[i];
    }
    return found;
}

const XrFreestandingEntryPlan *xr_native_package_find_entry(const XrNativePackagePlan *plan,
                                                            const char *xray_name) {
    const XrFreestandingEntryPlan *found = NULL;
    if (!plan || !xray_name)
        return NULL;
    for (uint32_t i = 0; i < plan->entry_count; i++) {
        if (!native_plan_name_matches(plan->entries[i].xray_name, xray_name))
            continue;
        if (found)
            return NULL;
        found = &plan->entries[i];
    }
    return found;
}

const char *xr_native_symbol_library(const XrNativeSymbol *symbol) {
    if (!symbol || !symbol->unit)
        return NULL;
    if (symbol->unit->output && symbol->unit->output[0])
        return symbol->unit->output;
    if (symbol->unit->kind == XR_NATIVE_UNIT_PLATFORM && symbol->unit->system_link_count == 1)
        return symbol->unit->system_links[0];
    if ((symbol->unit->kind == XR_NATIVE_UNIT_OBJECT ||
         symbol->unit->kind == XR_NATIVE_UNIT_STATIC_LIBRARY ||
         symbol->unit->kind == XR_NATIVE_UNIT_DYNAMIC_LIBRARY) &&
        symbol->unit->source_count == 1)
        return symbol->unit->sources[0];
    return NULL;
}

bool xr_native_package_resolve_layout(XrNativePackagePlan *plan, const char *xray_type,
                                      const XrAggregateLayout *layout) {
    bool matched = false;
    if (!plan || !xray_type || !layout)
        return false;
    for (uint32_t i = 0; i < plan->layout_count; i++) {
        XrNativeLayoutAssertion *assertion = &plan->layouts[i];
        const char *short_name = assertion->xray_type ? strrchr(assertion->xray_type, '.') : NULL;
        short_name = short_name ? short_name + 1 : assertion->xray_type;
        if (!assertion->xray_type ||
            (strcmp(assertion->xray_type, xray_type) != 0 && strcmp(short_name, xray_type) != 0))
            continue;
        native_free_string_array(assertion->field_names, assertion->field_count);
        xr_free(assertion->field_offsets);
        assertion->field_names = NULL;
        assertion->field_offsets = NULL;
        assertion->field_count = layout->field_count;
        if (layout->field_count > 0) {
            assertion->field_names =
                (char **) xr_calloc((size_t) layout->field_count, sizeof(char *));
            assertion->field_offsets =
                (uint32_t *) xr_calloc((size_t) layout->field_count, sizeof(uint32_t));
            if (!assertion->field_names || !assertion->field_offsets) {
                native_free_string_array(assertion->field_names, assertion->field_count);
                xr_free(assertion->field_offsets);
                assertion->field_names = NULL;
                assertion->field_offsets = NULL;
                assertion->field_count = 0;
                assertion->resolved = false;
                return false;
            }
            for (uint32_t fi = 0; fi < layout->field_count; fi++) {
                assertion->field_names[fi] = xr_strdup(
                    layout->field_names && layout->field_names[fi] ? layout->field_names[fi] : "");
                assertion->field_offsets[fi] = layout->fields[fi].offset;
                if (!assertion->field_names[fi])
                    return false;
            }
        }
        assertion->expected_size = layout->total_size;
        assertion->expected_align = layout->alignment;
        assertion->resolved = true;
        matched = true;
    }
    return matched;
}

bool xr_native_package_validate_symbol_arity(const XrNativePackagePlan *plan, const char *xray_name,
                                             uint32_t arity, char *errbuf, size_t errbuf_len) {
    const XrNativeSymbol *symbol = xr_native_package_find_symbol(plan, xray_name);
    if (!symbol) {
        /* Export/link/entry-only manifests do not seal the process C symbol
         * namespace. A present [native] table does: exploratory entries may
         * omit detailed contracts, but every mapped name is still explicit. */
        if (!plan || plan->audit_mode == XR_NATIVE_AUDIT_NONE)
            return true;
        if (errbuf && errbuf_len)
            snprintf(errbuf, errbuf_len,
                     "E-NATIVE-UNDECLARED-SYMBOL: extern '%s' is absent from NativePackagePlan",
                     xray_name ? xray_name : "?");
        return false;
    }
    if (!symbol->contract.complete)
        return true;
    if (symbol->contract.param_count != arity) {
        if (errbuf && errbuf_len)
            snprintf(errbuf, errbuf_len,
                     "E-NATIVE-CONTRACT: extern '%s' has arity %u but its contract describes %u",
                     xray_name ? xray_name : "?", arity, symbol->contract.param_count);
        return false;
    }
    return true;
}

const char *xr_native_audit_mode_name(XrNativeAuditMode mode) {
    switch (mode) {
        case XR_NATIVE_AUDIT_EXPLORATORY:
            return "exploratory";
        case XR_NATIVE_AUDIT_SHIPPING:
            return "shipping";
        default:
            return "none";
    }
}

const char *xr_native_unit_kind_name(XrNativeUnitKind kind) {
    switch (kind) {
        case XR_NATIVE_UNIT_C:
            return "c";
        case XR_NATIVE_UNIT_ASM:
            return "asm";
        case XR_NATIVE_UNIT_OBJECT:
            return "object";
        case XR_NATIVE_UNIT_STATIC_LIBRARY:
            return "static-library";
        case XR_NATIVE_UNIT_DYNAMIC_LIBRARY:
            return "dynamic-library";
        case XR_NATIVE_UNIT_PLATFORM:
            return "platform";
        default:
            return "unknown";
    }
}

const char *xr_native_param_access_name(XrNativeParamAccess access) {
    switch (access) {
        case XR_NATIVE_ACCESS_NONE:
            return "none";
        case XR_NATIVE_ACCESS_READ:
            return "read";
        case XR_NATIVE_ACCESS_WRITE:
            return "write";
        case XR_NATIVE_ACCESS_READWRITE:
            return "readwrite";
        default:
            return "unknown";
    }
}

void xr_native_package_explain(const XrNativePackagePlan *plan, FILE *out) {
    if (!out)
        out = stdout;
    if (!plan) {
        fprintf(out, "native-plan: none\n");
        return;
    }
    fprintf(out, "native-plan package=%s version=%s audit=%s vm=%s fingerprint=%016llx valid=%s\n",
            plan->name ? plan->name : "?", plan->version ? plan->version : "?",
            xr_native_audit_mode_name(plan->audit_mode),
            plan->vm_policy == XR_NATIVE_VM_VERIFIED_DYNAMIC ? "verified-dynamic" : "unsupported",
            (unsigned long long) plan->fingerprint, plan->valid ? "yes" : "no");
    if (!plan->valid) {
        fprintf(out, "error: %s\n", plan->error ? plan->error : "invalid native plan");
        return;
    }
    for (uint32_t i = 0; i < plan->unit_count; i++) {
        const XrNativeUnit *unit = &plan->units[i];
        fprintf(out, "unit %s kind=%s fingerprint=%016llx purpose=%s output=%s\n", unit->name,
                xr_native_unit_kind_name(unit->kind), (unsigned long long) unit->fingerprint,
                unit->purpose ? unit->purpose : "?", unit->output ? unit->output : "none");
        for (uint32_t si = 0; si < unit->source_count; si++)
            fprintf(out, "  source %s sha256=%s\n", unit->source_relpaths[si],
                    unit->source_hashes[si]);
    }
    for (uint32_t i = 0; i < plan->symbol_count; i++) {
        const XrNativeSymbol *symbol = &plan->symbols[i];
        fprintf(out, "symbol %s -> %s unit=%s contract=%s params=%u\n", symbol->xray_name,
                symbol->native_name, symbol->unit_name,
                symbol->contract.complete ? "complete" : "exploratory",
                symbol->contract.param_count);
        for (uint32_t pi = 0; pi < symbol->contract.param_count; pi++) {
            const XrNativeParamContract *param = &symbol->contract.params[pi];
            fprintf(out,
                    "  param %u access=%s nullable=%s length_from=%d escape=%u ownership=%u "
                    "rebind=%u relocate=%u shorten=%u invalidate=%u\n",
                    pi, xr_native_param_access_name(param->access), param->nullable ? "yes" : "no",
                    param->length_from, (unsigned) param->escape, (unsigned) param->ownership,
                    param->descriptor_rebind, param->may_relocate, param->may_shorten,
                    param->invalidates_views);
        }
    }
    for (uint32_t i = 0; i < plan->target_count; i++) {
        const XrNativeTargetPlan *target = &plan->targets[i];
        fprintf(out, "target %s profile=%s visibility=%s vm=%s cpu=%s\n", target->triple,
                target->profile, target->visibility ? target->visibility : "unit-default",
                target->vm_policy == XR_NATIVE_VM_VERIFIED_DYNAMIC ? "verified-dynamic"
                                                                   : "unsupported",
                target->cpu_feature ? target->cpu_feature : "baseline");
    }
    for (uint32_t i = 0; i < plan->export_count; i++)
        fprintf(out, "export.c %s -> %s visibility=%s header=%s\n", plan->exports[i].xray_name,
                plan->exports[i].symbol,
                plan->exports[i].visibility ? plan->exports[i].visibility : "default",
                plan->exports[i].header ? "yes" : "no");
    for (uint32_t i = 0; i < plan->link_symbol_count; i++)
        fprintf(out, "link.symbol %s section=%s used=%s weak=%s\n", plan->link_symbols[i].xray_name,
                plan->link_symbols[i].section ? plan->link_symbols[i].section : "none",
                plan->link_symbols[i].used ? "yes" : "no",
                plan->link_symbols[i].weak ? "yes" : "no");
    for (uint32_t i = 0; i < plan->entry_count; i++)
        fprintf(out, "freestanding.entry %s -> %s kind=%u abi=%s section=%s stub=%s\n",
                plan->entries[i].xray_name, plan->entries[i].symbol,
                (unsigned) plan->entries[i].kind,
                plan->entries[i].abi ? plan->entries[i].abi : "none", plan->entries[i].section,
                plan->entries[i].stub ? plan->entries[i].stub : "none");
}
