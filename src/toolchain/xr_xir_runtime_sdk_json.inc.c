/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk_json.inc.c - Allocation-free closed bundle JSON decoder
 *
 * KEY CONCEPT:
 *   Reads and writes charge the caller's cumulative ledger; only known members exist.
 */
#include "xr_xir_runtime_sdk_internal.h"
#include "../base/xjson_cursor.h"
#include <string.h>
static XrXirRuntimeSdkStatus sdk_resource_status(XrCompileResourceStatus status) {
    switch (status) {
    case XR_COMPILE_RESOURCE_OK: return XR_XIR_SDK_OK;
    case XR_COMPILE_RESOURCE_BUDGET: return XR_XIR_SDK_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_XIR_SDK_OUT_OF_MEMORY;
    default: return XR_XIR_SDK_INVALID;
    }
}
static XrJsonCursorStatus sdk_cursor_charge(void *context, uint64_t work) {
    switch (xr_compile_resources_work(context, work)) {
    case XR_COMPILE_RESOURCE_OK: return XR_JSON_CURSOR_OK;
    case XR_COMPILE_RESOURCE_BUDGET: return XR_JSON_CURSOR_BUDGET;
    case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_JSON_CURSOR_OUT_OF_MEMORY;
    default: return XR_JSON_CURSOR_INVALID;
    }
}
static XrXirRuntimeSdkStatus sdk_cursor_status(XrJsonCursorStatus status) {
    switch (status) {
    case XR_JSON_CURSOR_OK: return XR_XIR_SDK_OK;
    case XR_JSON_CURSOR_BUDGET: return XR_XIR_SDK_BUDGET;
    case XR_JSON_CURSOR_OUT_OF_MEMORY: return XR_XIR_SDK_OUT_OF_MEMORY;
    case XR_JSON_CURSOR_UNSUPPORTED: return XR_XIR_SDK_UNSUPPORTED;
    default: return XR_XIR_SDK_INVALID;
    }
}
static bool sdk_json_abi_entry(XrJsonCursor *json, const XrXirSdkAbiField *field) {
    static const char *const names[] = {"id", "value"};
    uint64_t seen = 0, values[2] = {0};
    bool more = true;
    if (!xr_json_cursor_take(json, '{')) return false;
    while (more) {
        uint32_t ordinal = 0;
        if (!xr_json_cursor_key(json, names, 2, &seen, &ordinal) ||
            !xr_json_cursor_u64(json, &values[ordinal]) || !xr_json_cursor_member_end(json, &more)) return false;
    }
    return seen == 3 && values[0] == field->id && values[1] == field->value ? true :
        xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
}
static bool sdk_json_abi(XrJsonCursor *json) {
    if (!xr_json_cursor_take(json, '[')) return false;
    for (uint32_t i = 0; i < sizeof(sdk_abi_fields) / sizeof(sdk_abi_fields[0]); ++i) {
        if ((i && !xr_json_cursor_take(json, ',')) || !sdk_json_abi_entry(json, &sdk_abi_fields[i])) return false;
    }
    return xr_json_cursor_take(json, ']');
}
static bool sdk_json_digest(XrJsonCursor *json, uint8_t output[32]) {
    const char *text = NULL;
    if (!xr_json_cursor_string(json, 64, &text)) return false;
    size_t length = 0;
    if (!xr_json_cursor_length(json, text, &length)) return false;
    if (length != 64) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    for (uint32_t i = 0; i < 64; ++i) {
        if (!xr_json_cursor_work(json, 1)) return false;
        uint8_t byte = (uint8_t)text[i];
        uint32_t digit = byte >= '0' && byte <= '9' ? (uint32_t)(byte - '0') :
            byte >= 'a' && byte <= 'f' ? (uint32_t)(byte - 'a' + 10) : 16u;
        if (digit == 16) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
        if (!(i & 1)) output[i / 2] = (uint8_t)(digit << 4);
        else output[i / 2] |= (uint8_t)digit;
    }
    return true;
}
static bool sdk_json_file(XrJsonCursor *json, XrXirSdkFile *file,
    const XrXirSdkRecipeFile *recipe) {
    static const char *const names[] = {"path", "kind", "length", "sha256"};
    uint64_t seen = 0, kind = 0;
    bool more = true;
    if (!xr_json_cursor_take(json, '{')) return false;
    while (more) {
        uint32_t ordinal = 0;
        if (!xr_json_cursor_key(json, names, 4, &seen, &ordinal)) return false;
        if (ordinal == 0 && !xr_json_cursor_string(json, XR_XIR_SDK_PATH_LIMIT, &file->path)) return false;
        if (ordinal == 1 && !xr_json_cursor_u64(json, &kind)) return false;
        if (ordinal == 2 && !xr_json_cursor_u64(json, &file->length)) return false;
        if (ordinal == 3 && !sdk_json_digest(json, file->digest)) return false;
        if (!xr_json_cursor_member_end(json, &more)) return false;
    }
    if (seen != 15 || !xr_json_cursor_equal(json, file->path, recipe->path) || kind != recipe->kind)
        return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    if (file->length > XR_XIR_SDK_FILE_BYTES) return xr_json_cursor_fail(json, XR_JSON_CURSOR_BUDGET);
    file->kind = (uint32_t)kind;
    return true;
}
static bool sdk_json_files(XrJsonCursor *json, XrXirSdkManifest *manifest) {
    uint32_t count = (uint32_t)(sizeof(sdk_recipe_files) / sizeof(sdk_recipe_files[0]));
    if (!xr_json_cursor_take(json, '[')) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if ((i && !xr_json_cursor_take(json, ',')) ||
            !sdk_json_file(json, &manifest->files[i], &sdk_recipe_files[i])) return false;
        if (manifest->files[i].length > XR_XIR_SDK_BUNDLE_BYTES - manifest->bundle_bytes)
            return xr_json_cursor_fail(json, XR_JSON_CURSOR_BUDGET);
        manifest->bundle_bytes += manifest->files[i].length;
    }
    manifest->file_count = count;
    return xr_json_cursor_take(json, ']');
}
static bool sdk_json_libraries(XrJsonCursor *json) {
    const char *name = NULL;
    return xr_json_cursor_take(json, '[') && xr_json_cursor_string(json, 128, &name) &&
        xr_json_cursor_equal(json, name, "kernel32") && xr_json_cursor_take(json, ']');
}
static bool sdk_json_prefix(XrJsonCursor *json, const XrXirSdkManifest *manifest) {
    static const uint32_t fixed[] = {2, XR_XIR_CHECKED_SCHEMA, XR_XIR_CHECKED_CONTRACT,
        XR_XIR_VALUE_ABI_VERSION, XR_XIR_CALL_ABI_VERSION, XR_XIR_PROGRAM_ABI_VERSION, 1, 1, 1, 11};
    for (uint32_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        if (manifest->prefix[i] != fixed[i]) return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    const uint32_t *p = manifest->prefix;
    if (p[10] < 1 || p[10] > 2 || (p[11] & ~7u) || (p[11] & 5u) == 5u ||
        p[12] < 1 || p[12] > 2 || p[13] > 1 || p[14] < 1 || p[14] > 4 || p[15] != 1 || p[16] != 1)
        return xr_json_cursor_fail(json, XR_JSON_CURSOR_INVALID);
    if (p[11] || p[12] != 1) return xr_json_cursor_fail(json, XR_JSON_CURSOR_UNSUPPORTED);
    return xr_json_cursor_equal(json, manifest->target_triple, "x86_64-windows-msvc") &&
        xr_json_cursor_equal(json, manifest->abi_recipe, "xray:xir-runtime-abi-measurements:v1") &&
        xr_json_cursor_equal(json, manifest->closure_recipe, "xray:xir-runtime-recipe:windows-x86_64-hosted:v1");
}
static XrXirRuntimeSdkStatus sdk_json_parse(char *bytes, size_t length, XrCompileResources *resources,
    XrXirSdkManifest *manifest, size_t *failure_offset) {
    static const char *const names[] = {"schema", "wire", "semantic", "value_abi", "call_abi",
        "program_abi", "architecture", "object_format", "hosted", "c_dialect", "crt",
        "sanitizers", "allocator", "assertions", "build_provider", "abi_recipe_version",
        "closure_recipe_version", "target_triple", "abi_recipe", "closure_recipe",
        "abi_measurements", "files", "system_libraries"};
    if (!bytes || !manifest || !length || !resources) return XR_XIR_SDK_INVALID;
    if (length > XR_XIR_SDK_MANIFEST_LIMIT) return XR_XIR_SDK_BUDGET;
    XrJsonCursor json = xr_json_cursor_make(bytes, length, resources, sdk_cursor_charge);
    uint64_t seen = 0;
    bool more = true;
    if (!xr_json_cursor_work(&json, sizeof(*manifest))) return sdk_cursor_status(json.status);
    memset(manifest, 0, sizeof(*manifest));
    if (!xr_json_cursor_take(&json, '{')) more = false;
    while (more && json.status == XR_JSON_CURSOR_OK) {
        uint32_t ordinal = 0;
        if (!xr_json_cursor_key(&json, names, 23, &seen, &ordinal)) break;
        if (ordinal < 17) {
            uint64_t value = 0;
            if (!xr_json_cursor_u64(&json, &value)) break;
            if (value > UINT32_MAX) { xr_json_cursor_fail(&json, XR_JSON_CURSOR_BUDGET); break; }
            manifest->prefix[ordinal] = (uint32_t)value;
        } else if (ordinal < 20) {
            const char **string = ordinal == 17 ? &manifest->target_triple :
                ordinal == 18 ? &manifest->abi_recipe : &manifest->closure_recipe;
            if (!xr_json_cursor_string(&json, 1024, string)) break;
        } else if (ordinal == 20 && !sdk_json_abi(&json)) break;
        else if (ordinal == 21 && !sdk_json_files(&json, manifest)) break;
        else if (ordinal == 22 && !sdk_json_libraries(&json)) break;
        if (!xr_json_cursor_member_end(&json, &more)) break;
    }
    if (json.status == XR_JSON_CURSOR_OK && (seen != ((UINT64_C(1) << 23) - 1) ||
        !xr_json_cursor_space(&json) || json.cursor != json.end || !sdk_json_prefix(&json, manifest)))
        xr_json_cursor_fail(&json, XR_JSON_CURSOR_INVALID);
    if (failure_offset) *failure_offset = (size_t)(json.cursor - json.begin);
    return sdk_cursor_status(json.status);
}
