/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_xir_runtime_sdk_identity.inc.c - Canonical runtime bundle framing
 *
 * KEY CONCEPT:
 *   Only validated values enter the digest; padding and addresses never do.
 */
static bool sdk_identity_bytes(SdkJson *json, XrSHA256Context *hash,
    const void *bytes, size_t length) {
    if (!sdk_json_work(json, length)) return false;
    xr_sha256_update(hash, bytes, length);
    return true;
}
static bool sdk_identity_integer(SdkJson *json, XrSHA256Context *hash,
    uint64_t value, uint32_t width) {
    uint8_t bytes[8];
    if (!sdk_json_work(json, width)) return false;
    for (uint32_t i = 0; i < width; ++i) bytes[i] = (uint8_t)(value >> (i * 8));
    return sdk_identity_bytes(json, hash, bytes, width);
}
static bool sdk_identity_string(SdkJson *json, XrSHA256Context *hash, const char *text) {
    size_t length = 0;
    if (!sdk_json_length(json, text, &length)) return false;
    return sdk_identity_integer(json, hash, length, 4) && sdk_identity_bytes(json, hash, text, length);
}
static bool sdk_identity(SdkJson *json, XrXirSdkManifest *manifest) {
    static const char domain[] = "xray:xir-runtime-sdk:v1";
    XrSHA256Context hash;
    if (!sdk_json_work(json, 1)) return false;
    xr_sha256_init(&hash);
    if (!sdk_identity_bytes(json, &hash, domain, sizeof(domain) - 1)) return false;
    for (uint32_t i = 0; i < 17; ++i)
        if (!sdk_identity_integer(json, &hash, manifest->prefix[i], 4)) return false;
    if (!sdk_identity_string(json, &hash, manifest->target_triple) ||
        !sdk_identity_string(json, &hash, manifest->abi_recipe) ||
        !sdk_identity_string(json, &hash, manifest->closure_recipe)) return false;
    uint32_t count = (uint32_t)(sizeof(sdk_abi_fields) / sizeof(sdk_abi_fields[0]));
    if (!sdk_identity_integer(json, &hash, count, 4)) return false;
    for (uint32_t i = 0; i < count; ++i)
        if (!sdk_identity_integer(json, &hash, sdk_abi_fields[i].id, 4) ||
            !sdk_identity_integer(json, &hash, sdk_abi_fields[i].value, 4)) return false;
    if (!sdk_identity_integer(json, &hash, manifest->file_count, 4)) return false;
    for (uint32_t i = 0; i < manifest->file_count; ++i) {
        const XrXirSdkFile *file = &manifest->files[i];
        if (!sdk_identity_string(json, &hash, file->path) ||
            !sdk_identity_integer(json, &hash, file->kind, 4) ||
            !sdk_identity_integer(json, &hash, file->length, 8) ||
            !sdk_identity_bytes(json, &hash, file->digest, 32)) return false;
    }
    if (!sdk_identity_integer(json, &hash, 1, 4) ||
        !sdk_identity_string(json, &hash, "kernel32")) return false;
    if (!sdk_json_work(json, 1)) return false;
    xr_sha256_final(&hash, manifest->digest);
    return true;
}
