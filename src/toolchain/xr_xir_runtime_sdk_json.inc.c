/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk_json.inc.c - Allocation-free closed bundle JSON decoder
 *
 * KEY CONCEPT:
 *   Each consumed byte is charged to one work counter; only known members exist.
 */
#include "xr_xir_runtime_sdk_internal.h"
#include <string.h>
typedef struct SdkJson {
    char *begin, *cursor, *end;
    uint64_t remaining, initial;
    XrXirRuntimeSdkStatus status;
} SdkJson;
static bool sdk_json_fail(SdkJson *json, XrXirRuntimeSdkStatus status) {
    if (json->status == XR_XIR_SDK_OK) json->status = status;
    return false;
}
static bool sdk_json_work(SdkJson *json, uint64_t work) {
    if (json->status != XR_XIR_SDK_OK) return false;
    if (work > json->remaining) return sdk_json_fail(json, XR_XIR_SDK_BUDGET);
    json->remaining -= work;
    return true;
}
static bool sdk_json_byte(SdkJson *json, uint8_t *output) {
    if (json->cursor == json->end) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    if (!sdk_json_work(json, 1)) return false;
    *output = (uint8_t)*json->cursor++;
    return true;
}
static bool sdk_json_space(SdkJson *json) {
    while (json->cursor != json->end && (*json->cursor == ' ' || *json->cursor == '\t' ||
           *json->cursor == '\n' || *json->cursor == '\r')) {
        if (!sdk_json_work(json, 1)) return false;
        ++json->cursor;
    }
    return json->status == XR_XIR_SDK_OK;
}
static bool sdk_json_take(SdkJson *json, char token) {
    uint8_t actual = 0;
    return sdk_json_space(json) && sdk_json_byte(json, &actual) &&
        (actual == (uint8_t)token || sdk_json_fail(json, XR_XIR_SDK_INVALID));
}
static bool sdk_json_equal(SdkJson *json, const char *left, const char *right) {
    for (;;) {
        if (!sdk_json_work(json, 1)) return false;
        if (*left != *right) return false;
        if (!*left++) return true;
        ++right;
    }
}
static bool sdk_json_hex4(SdkJson *json, uint32_t *output) {
    uint32_t code = 0;
    for (uint32_t digit = 0; digit < 4; ++digit) {
        uint8_t byte = 0;
        if (!sdk_json_byte(json, &byte)) return false;
        uint32_t value = byte >= '0' && byte <= '9' ? (uint32_t)(byte - '0') :
            byte >= 'a' && byte <= 'f' ? (uint32_t)(byte - 'a' + 10) :
            byte >= 'A' && byte <= 'F' ? (uint32_t)(byte - 'A' + 10) : 16u;
        if (value == 16) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        code = code * 16 + value;
    }
    *output = code;
    return true;
}
static bool sdk_json_unicode(SdkJson *json, uint32_t *output) {
    uint32_t code = 0;
    if (!sdk_json_hex4(json, &code)) return false;
    if (code >= 0xd800 && code <= 0xdbff) {
        uint8_t slash = 0, letter = 0;
        uint32_t low = 0;
        if (!sdk_json_byte(json, &slash) || !sdk_json_byte(json, &letter) ||
            slash != '\\' || letter != 'u' || !sdk_json_hex4(json, &low) ||
            low < 0xdc00 || low > 0xdfff) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        code = 0x10000 + (code - 0xd800) * 0x400 + low - 0xdc00;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
        return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    }
    if (!code) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    *output = code;
    return true;
}
static size_t sdk_json_encode(uint32_t code, char *output) {
    if (code < 0x80) { output[0] = (char)code; return 1; }
    if (code < 0x800) {
        output[0] = (char)(0xc0 | (code >> 6)); output[1] = (char)(0x80 | (code & 63)); return 2;
    }
    if (code < 0x10000) {
        output[0] = (char)(0xe0 | (code >> 12)); output[1] = (char)(0x80 | ((code >> 6) & 63));
        output[2] = (char)(0x80 | (code & 63)); return 3;
    }
    output[0] = (char)(0xf0 | (code >> 18)); output[1] = (char)(0x80 | ((code >> 12) & 63));
    output[2] = (char)(0x80 | ((code >> 6) & 63)); output[3] = (char)(0x80 | (code & 63)); return 4;
}
static bool sdk_json_utf8(SdkJson *json, uint8_t first, uint32_t *output) {
    uint32_t count = first >= 0xc2 && first <= 0xdf ? 1u :
        first >= 0xe0 && first <= 0xef ? 2u : first >= 0xf0 && first <= 0xf4 ? 3u : 0u;
    if (!count) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    uint32_t code = first & (count == 1 ? 31u : count == 2 ? 15u : 7u);
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t next = 0;
        if (!sdk_json_byte(json, &next) || (next & 0xc0) != 0x80)
            return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        code = code * 64 + (next & 63);
    }
    uint32_t minimum = count == 1 ? 0x80u : count == 2 ? 0x800u : 0x10000u;
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    *output = code;
    return true;
}
static bool sdk_json_string(SdkJson *json, size_t limit, const char **output) {
    if (!sdk_json_take(json, '"')) return false;
    char *begin = json->cursor, *write = begin;
    for (;;) {
        uint8_t byte = 0;
        if (!sdk_json_byte(json, &byte)) return false;
        if (byte == '"') { *write = 0; *output = begin; return true; }
        if (byte < 32) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        uint32_t code = byte;
        if (byte == '\\') {
            if (!sdk_json_byte(json, &byte)) return false;
            if (byte == 'u') {
                if (!sdk_json_unicode(json, &code)) return false;
            } else if (byte == '"' || byte == '\\' || byte == '/') code = byte;
            else if (byte == 'b') code = '\b';
            else if (byte == 'f') code = '\f';
            else if (byte == 'n') code = '\n';
            else if (byte == 'r') code = '\r';
            else if (byte == 't') code = '\t';
            else return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        } else if (byte >= 128 && !sdk_json_utf8(json, byte, &code)) return false;
        size_t bytes = sdk_json_encode(code, write);
        if ((size_t)(write - begin) > limit || bytes > limit - (size_t)(write - begin))
            return sdk_json_fail(json, XR_XIR_SDK_BUDGET);
        write += bytes;
    }
}
static bool sdk_json_u64(SdkJson *json, uint64_t *output) {
    if (!sdk_json_space(json) || json->cursor == json->end ||
        *json->cursor < '0' || *json->cursor > '9') return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    bool zero = *json->cursor == '0';
    uint64_t value = 0;
    uint32_t digits = 0;
    while (json->cursor != json->end && *json->cursor >= '0' && *json->cursor <= '9') {
        uint8_t byte = 0;
        if (!sdk_json_byte(json, &byte)) return false;
        if (zero && digits) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        uint32_t digit = byte - '0';
        if (value > (UINT64_MAX - digit) / 10) return sdk_json_fail(json, XR_XIR_SDK_BUDGET);
        value = value * 10 + digit;
        ++digits;
    }
    *output = value;
    return true;
}
static bool sdk_json_key(SdkJson *json, const char *const *names, uint32_t count,
    uint64_t *seen, uint32_t *ordinal) {
    const char *key = NULL;
    if (!sdk_json_string(json, 128, &key) || !sdk_json_take(json, ':')) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!sdk_json_equal(json, key, names[i])) continue;
        if (*seen & (UINT64_C(1) << i)) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        *seen |= UINT64_C(1) << i;
        *ordinal = i;
        return true;
    }
    return sdk_json_fail(json, XR_XIR_SDK_INVALID);
}
static bool sdk_json_member_end(SdkJson *json, bool *more) {
    if (!sdk_json_space(json) || json->cursor == json->end)
        return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    char token = *json->cursor;
    if (token != ',' && token != '}') return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    if (!sdk_json_take(json, token)) return false;
    *more = token == ',';
    return true;
}
static bool sdk_json_abi_entry(SdkJson *json, const XrXirSdkAbiField *field) {
    static const char *const names[] = {"id", "value"};
    uint64_t seen = 0, values[2] = {0};
    bool more = true;
    if (!sdk_json_take(json, '{')) return false;
    while (more) {
        uint32_t ordinal = 0;
        if (!sdk_json_key(json, names, 2, &seen, &ordinal) ||
            !sdk_json_u64(json, &values[ordinal]) || !sdk_json_member_end(json, &more)) return false;
    }
    return seen == 3 && values[0] == field->id && values[1] == field->value ? true :
        sdk_json_fail(json, XR_XIR_SDK_INVALID);
}
static bool sdk_json_abi(SdkJson *json) {
    if (!sdk_json_take(json, '[')) return false;
    for (uint32_t i = 0; i < sizeof(sdk_abi_fields) / sizeof(sdk_abi_fields[0]); ++i) {
        if ((i && !sdk_json_take(json, ',')) || !sdk_json_abi_entry(json, &sdk_abi_fields[i])) return false;
    }
    return sdk_json_take(json, ']');
}
static bool sdk_json_digest(SdkJson *json, uint8_t output[32]) {
    const char *text = NULL;
    if (!sdk_json_string(json, 64, &text)) return false;
    if (strlen(text) != 64) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    for (uint32_t i = 0; i < 64; ++i) {
        if (!sdk_json_work(json, 1)) return false;
        uint8_t byte = (uint8_t)text[i];
        uint32_t digit = byte >= '0' && byte <= '9' ? (uint32_t)(byte - '0') :
            byte >= 'a' && byte <= 'f' ? (uint32_t)(byte - 'a' + 10) : 16u;
        if (digit == 16) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
        if (!(i & 1)) output[i / 2] = (uint8_t)(digit << 4);
        else output[i / 2] |= (uint8_t)digit;
    }
    return true;
}
static bool sdk_json_file(SdkJson *json, XrXirSdkFile *file,
    const XrXirSdkRecipeFile *recipe) {
    static const char *const names[] = {"path", "kind", "length", "sha256"};
    uint64_t seen = 0, kind = 0;
    bool more = true;
    if (!sdk_json_take(json, '{')) return false;
    while (more) {
        uint32_t ordinal = 0;
        if (!sdk_json_key(json, names, 4, &seen, &ordinal)) return false;
        if (ordinal == 0 && !sdk_json_string(json, XR_XIR_SDK_PATH_LIMIT, &file->path)) return false;
        if (ordinal == 1 && !sdk_json_u64(json, &kind)) return false;
        if (ordinal == 2 && !sdk_json_u64(json, &file->length)) return false;
        if (ordinal == 3 && !sdk_json_digest(json, file->digest)) return false;
        if (!sdk_json_member_end(json, &more)) return false;
    }
    if (seen != 15 || !sdk_json_equal(json, file->path, recipe->path) || kind != recipe->kind)
        return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    if (file->length > XR_XIR_SDK_FILE_BYTES) return sdk_json_fail(json, XR_XIR_SDK_BUDGET);
    file->kind = (uint32_t)kind;
    return true;
}
static bool sdk_json_files(SdkJson *json, XrXirSdkManifest *manifest) {
    uint32_t count = (uint32_t)(sizeof(sdk_recipe_files) / sizeof(sdk_recipe_files[0]));
    if (!sdk_json_take(json, '[')) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if ((i && !sdk_json_take(json, ',')) ||
            !sdk_json_file(json, &manifest->files[i], &sdk_recipe_files[i])) return false;
        if (manifest->files[i].length > XR_XIR_SDK_BUNDLE_BYTES - manifest->bundle_bytes)
            return sdk_json_fail(json, XR_XIR_SDK_BUDGET);
        manifest->bundle_bytes += manifest->files[i].length;
    }
    manifest->file_count = count;
    return sdk_json_take(json, ']');
}
static bool sdk_json_libraries(SdkJson *json) {
    const char *name = NULL;
    return sdk_json_take(json, '[') && sdk_json_string(json, 128, &name) &&
        sdk_json_equal(json, name, "kernel32") && sdk_json_take(json, ']');
}
static bool sdk_json_prefix(SdkJson *json, const XrXirSdkManifest *manifest) {
    static const uint32_t fixed[] = {2, XR_XIR_CHECKED_SCHEMA, XR_XIR_CHECKED_CONTRACT,
        XR_XIR_VALUE_ABI_VERSION, XR_XIR_CALL_ABI_VERSION, XR_XIR_PROGRAM_ABI_VERSION, 1, 1, 1, 11};
    for (uint32_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i)
        if (manifest->prefix[i] != fixed[i]) return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    const uint32_t *p = manifest->prefix;
    if (p[10] < 1 || p[10] > 2 || (p[11] & ~7u) || (p[11] & 5u) == 5u ||
        p[12] < 1 || p[12] > 2 || p[13] > 1 || p[14] < 1 || p[14] > 4 || p[15] != 1 || p[16] != 1)
        return sdk_json_fail(json, XR_XIR_SDK_INVALID);
    if (p[11] || p[12] != 1) return sdk_json_fail(json, XR_XIR_SDK_UNSUPPORTED);
    return sdk_json_equal(json, manifest->target_triple, "x86_64-windows-msvc") &&
        sdk_json_equal(json, manifest->abi_recipe, "xray:xir-runtime-abi-measurements:v1") &&
        sdk_json_equal(json, manifest->closure_recipe, "xray:xir-runtime-recipe:windows-x86_64-hosted:v1");
}
static XrXirRuntimeSdkStatus sdk_json_parse(char *bytes, size_t length, uint64_t work,
    XrXirSdkManifest *manifest, size_t *failure_offset) {
    static const char *const names[] = {"schema", "wire", "semantic", "value_abi", "call_abi",
        "program_abi", "architecture", "object_format", "hosted", "c_dialect", "crt",
        "sanitizers", "allocator", "assertions", "build_provider", "abi_recipe_version",
        "closure_recipe_version", "target_triple", "abi_recipe", "closure_recipe",
        "abi_measurements", "files", "system_libraries"};
    if (!bytes || !manifest || !length) return XR_XIR_SDK_INVALID;
    if (length > XR_XIR_SDK_MANIFEST_LIMIT || work > XR_XIR_SDK_WORK_LIMIT) return XR_XIR_SDK_BUDGET;
    SdkJson json = {bytes, bytes, bytes + length, work, work, XR_XIR_SDK_OK};
    uint64_t seen = 0;
    bool more = true;
    memset(manifest, 0, sizeof(*manifest));
    if (!sdk_json_take(&json, '{')) more = false;
    while (more && json.status == XR_XIR_SDK_OK) {
        uint32_t ordinal = 0;
        if (!sdk_json_key(&json, names, 23, &seen, &ordinal)) break;
        if (ordinal < 17) {
            uint64_t value = 0;
            if (!sdk_json_u64(&json, &value)) break;
            if (value > UINT32_MAX) { sdk_json_fail(&json, XR_XIR_SDK_BUDGET); break; }
            manifest->prefix[ordinal] = (uint32_t)value;
        } else if (ordinal < 20) {
            const char **string = ordinal == 17 ? &manifest->target_triple :
                ordinal == 18 ? &manifest->abi_recipe : &manifest->closure_recipe;
            if (!sdk_json_string(&json, 1024, string)) break;
        } else if (ordinal == 20 && !sdk_json_abi(&json)) break;
        else if (ordinal == 21 && !sdk_json_files(&json, manifest)) break;
        else if (ordinal == 22 && !sdk_json_libraries(&json)) break;
        if (!sdk_json_member_end(&json, &more)) break;
    }
    if (json.status == XR_XIR_SDK_OK && (seen != ((UINT64_C(1) << 23) - 1) ||
        !sdk_json_space(&json) || json.cursor != json.end || !sdk_json_prefix(&json, manifest)))
        sdk_json_fail(&json, XR_XIR_SDK_INVALID);
    manifest->work_used = json.initial - json.remaining;
    if (failure_offset) *failure_offset = (size_t)(json.cursor - json.begin);
    return json.status;
}
