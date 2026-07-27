/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_runtime_manifest.c - Exact target/ABI/provider runtime artifact resolver
 */

#include "xtc_runtime_manifest.h"

#include "../../base/xfileio.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"
#include "../../shared/xr_crypto_core.h"

#include <stdio.h>
#include <string.h>

#ifndef XTC_STATIC_LIBRARY_PREFIX
#ifdef XR_OS_WINDOWS
#define XTC_STATIC_LIBRARY_PREFIX ""
#else
#define XTC_STATIC_LIBRARY_PREFIX "lib"
#endif
#endif
#ifndef XTC_STATIC_LIBRARY_SUFFIX
#ifdef XR_OS_WINDOWS
#define XTC_STATIC_LIBRARY_SUFFIX ".lib"
#else
#define XTC_STATIC_LIBRARY_SUFFIX ".a"
#endif
#endif

static void xtc_runtime_error(char *err, size_t err_size, const char *format, const char *arg) {
    if (!err || err_size == 0)
        return;
    if (arg)
        snprintf(err, err_size, format, arg);
    else
        snprintf(err, err_size, "%s", format);
}

static bool xtc_runtime_dirname_in_place(char *path) {
    char *slash = path ? strrchr(path, '/') : NULL;
    char *backslash = path ? strrchr(path, '\\') : NULL;
    char *sep = slash;
    if (backslash && (!sep || backslash > sep))
        sep = backslash;
    if (!sep)
        return false;
    if (sep == path)
        sep[1] = '\0';
    else
        *sep = '\0';
    return true;
}

static bool xtc_runtime_install_root(const char *program_hint, char *out, size_t out_size) {
    char exe[1200];
    char marker[1400];
    (void) program_hint;
    if (xr_proc_self_exe_path(exe, sizeof(exe)) != 0 || !xtc_runtime_dirname_in_place(exe) ||
        !xtc_runtime_dirname_in_place(exe))
        return false;
    int written =
        snprintf(marker, sizeof(marker), "%s/share/xray/install/install-marker.json", exe);
    if (written < 0 || (size_t) written >= sizeof(marker) || !xr_fs_is_file(marker))
        return false;
    written = snprintf(out, out_size, "%s", exe);
    return written >= 0 && (size_t) written < out_size;
}

static void xtc_sha256_hex(const uint8_t digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static bool xtc_sha256_file(const char *path, char out[65]) {
    size_t size = 0;
    char *data = xr_file_read_all(path, "rb", &size);
    if (!data)
        return false;
    uint8_t digest[32];
    xr_sha256((const uint8_t *) data, size, digest);
    xr_free(data);
    xtc_sha256_hex(digest, out);
    return true;
}

static bool xtc_runtime_provider_allowed(XrJsonValue *providers, XrToolchainProviderId provider) {
    const char *name = xtc_provider_name(provider);
    int count = xjson_array_len(providers);
    for (int i = 0; i < count; i++) {
        XrJsonValue *value = xjson_array_get(providers, i);
        if (xjson_is_string(value) && strcmp(value->as.string, name) == 0)
            return true;
    }
    return false;
}

static bool xtc_runtime_safe_relative_file(const char *path) {
    return path && path[0] && !strchr(path, '/') && !strchr(path, '\\') && strcmp(path, ".") != 0 &&
           strcmp(path, "..") != 0;
}

static bool xtc_runtime_parse_artifacts(XrJsonValue *array, const char *dir,
                                        XrRuntimeArtifactSet *out, char *err, size_t err_size) {
    int count = xjson_array_len(array);
    if (count <= 0 || count > XTC_RUNTIME_MAX_ARTIFACTS) {
        xtc_runtime_error(err, err_size, "runtime manifest has invalid artifact count", NULL);
        return false;
    }
    for (int i = 0; i < count; i++) {
        XrJsonValue *item = xjson_array_get(array, i);
        const char *id = xjson_get_string(item, "id");
        const char *kind = xjson_get_string(item, "kind");
        const char *path = xjson_get_string(item, "path");
        const char *sha256 = xjson_get_string(item, "sha256");
        if (!id || !kind || !path || !sha256 || strlen(sha256) != 64 ||
            !xtc_runtime_safe_relative_file(path)) {
            xtc_runtime_error(err, err_size, "runtime manifest contains an invalid artifact", NULL);
            return false;
        }
        XrRuntimeArtifact *artifact = &out->artifacts[out->artifact_count];
        snprintf(artifact->id, sizeof(artifact->id), "%s", id);
        snprintf(artifact->kind, sizeof(artifact->kind), "%s", kind);
        int written = snprintf(artifact->path, sizeof(artifact->path), "%s/%s", dir, path);
        if (written < 0 || (size_t) written >= sizeof(artifact->path) ||
            !xr_fs_is_file(artifact->path)) {
            xtc_runtime_error(err, err_size, "runtime artifact is missing: %s", path);
            return false;
        }
        char actual[65];
        if (!xtc_sha256_file(artifact->path, actual) || strcmp(actual, sha256) != 0) {
            xtc_runtime_error(err, err_size, "runtime artifact digest mismatch: %s", path);
            return false;
        }
        snprintf(artifact->sha256, sizeof(artifact->sha256), "%s", actual);
        out->artifact_count++;
    }
    return true;
}

static bool xtc_runtime_parse_system_libraries(XrJsonValue *array, XrRuntimeArtifactSet *out) {
    int count = xjson_array_len(array);
    if (count < 0 || count > XTC_RUNTIME_MAX_SYSTEM_LIBS)
        return false;
    for (int i = 0; i < count; i++) {
        XrJsonValue *value = xjson_array_get(array, i);
        if (!xjson_is_string(value) || !value->as.string[0] || strchr(value->as.string, '/') ||
            strchr(value->as.string, '\\'))
            return false;
        snprintf(out->system_libraries[out->system_library_count++],
                 sizeof(out->system_libraries[0]), "%s", value->as.string);
    }
    return true;
}

static bool xtc_runtime_load_installed(const char *root, const XrToolchainTarget *target,
                                       XrToolchainProviderId provider, XrRuntimeArtifactSet *out,
                                       char *err, size_t err_size) {
    char dir[1200];
    int written = snprintf(dir, sizeof(dir), "%s/lib/xray/aot/%s", root, target->name);
    if (written < 0 || (size_t) written >= sizeof(dir))
        return false;
    written = snprintf(out->manifest_path, sizeof(out->manifest_path), "%s/manifest.json", dir);
    if (written < 0 || (size_t) written >= sizeof(out->manifest_path) ||
        !xr_fs_is_file(out->manifest_path)) {
        xtc_runtime_error(err, err_size, "runtime manifest is missing for target '%s'",
                          target->name);
        return false;
    }
    size_t size = 0;
    char *json = xr_file_read_all(out->manifest_path, "rb", &size);
    XrJsonValue *doc = json ? xjson_parse(json, size) : NULL;
    if (!doc || !xjson_is_object(doc) || xjson_get_int(doc, "schema") != 1 ||
        xjson_get_int(doc, "sdkAbi") != 1) {
        xjson_free(doc);
        xr_free(json);
        xtc_runtime_error(err, err_size, "runtime manifest is invalid", NULL);
        return false;
    }
    const char *manifest_target = xjson_get_string(doc, "target");
    const char *object_format = xjson_get_string(doc, "objectFormat");
    XrJsonValue *providers = xjson_get_array(doc, "providers");
    XrJsonValue *artifacts = xjson_get_array(doc, "artifacts");
    XrJsonValue *libraries = xjson_get_array(doc, "systemLibraries");
    if (!manifest_target || strcmp(manifest_target, target->name) != 0 || !object_format ||
        !providers || !artifacts || !libraries ||
        !xtc_runtime_provider_allowed(providers, provider)) {
        xjson_free(doc);
        xr_free(json);
        xtc_runtime_error(err, err_size, "runtime manifest does not match target/provider ABI",
                          NULL);
        return false;
    }
    out->schema = 1;
    out->sdk_abi = 1;
    snprintf(out->target, sizeof(out->target), "%s", manifest_target);
    snprintf(out->object_format, sizeof(out->object_format), "%s", object_format);
    written = snprintf(out->public_include, sizeof(out->public_include), "%s/include/xray", root);
    if (written < 0 || (size_t) written >= sizeof(out->public_include)) {
        xjson_free(doc);
        xr_free(json);
        return false;
    }
    written = snprintf(out->private_aot_include, sizeof(out->private_aot_include),
                       "%s/lib/xray/sdk/src/aot", root);
    if (written < 0 || (size_t) written >= sizeof(out->private_aot_include) ||
        !xr_fs_is_dir(out->public_include) || !xr_fs_is_dir(out->private_aot_include)) {
        xjson_free(doc);
        xr_free(json);
        xtc_runtime_error(err, err_size, "Xray SDK headers are missing", NULL);
        return false;
    }
    uint8_t digest[32];
    xr_sha256((const uint8_t *) json, size, digest);
    char digest_hex[65];
    xtc_sha256_hex(digest, digest_hex);
    snprintf(out->sdk_digest, sizeof(out->sdk_digest), "sha256:%s", digest_hex);
    bool ok = xtc_runtime_parse_artifacts(artifacts, dir, out, err, err_size) &&
              xtc_runtime_parse_system_libraries(libraries, out);
    xjson_free(doc);
    xr_free(json);
    return ok;
}

static bool xtc_runtime_load_build_tree(const XrToolchainTarget *target,
                                        XrToolchainProviderId provider, XrRuntimeArtifactSet *out,
                                        char *err, size_t err_size) {
#if defined(XRT_BUILD_LIB_DIR) && defined(XRT_AOT_INCLUDE_DIR) &&                                  \
    defined(XRT_SOURCE_INCLUDE_DIR) && defined(XTC_BUILD_HOST_TARGET)
    if (strcmp(target->name, XTC_BUILD_HOST_TARGET) != 0) {
        xtc_runtime_error(err, err_size, "build tree has no runtime for target '%s'", target->name);
        return false;
    }
    (void) provider;
    out->schema = 1;
    out->sdk_abi = 1;
    snprintf(out->target, sizeof(out->target), "%s", target->name);
#if defined(XR_OS_WINDOWS)
    snprintf(out->object_format, sizeof(out->object_format), "%s", "coff");
#elif defined(XR_OS_MACOS)
    snprintf(out->object_format, sizeof(out->object_format), "%s", "macho");
#else
    snprintf(out->object_format, sizeof(out->object_format), "%s", "elf");
#endif
    snprintf(out->public_include, sizeof(out->public_include), "%s", XRT_SOURCE_INCLUDE_DIR);
    snprintf(out->private_aot_include, sizeof(out->private_aot_include), "%s", XRT_AOT_INCLUDE_DIR);
    static const char *const names[] = {"xray_aot_core", "xray_rt_coro"};
    for (size_t i = 0; i < 2; i++) {
        XrRuntimeArtifact *artifact = &out->artifacts[out->artifact_count];
        snprintf(artifact->id, sizeof(artifact->id), "xray-%s-%s-v1",
                 i == 0 ? "aot-core" : "rt-coro", target->name);
        snprintf(artifact->kind, sizeof(artifact->kind), "%s", "static-library");
        snprintf(artifact->path, sizeof(artifact->path), "%s/%s%s%s", XRT_BUILD_LIB_DIR,
                 XTC_STATIC_LIBRARY_PREFIX, names[i], XTC_STATIC_LIBRARY_SUFFIX);
        if (!xtc_sha256_file(artifact->path, artifact->sha256)) {
            xtc_runtime_error(err, err_size, "build-tree runtime artifact is missing: %s",
                              artifact->path);
            return false;
        }
        out->artifact_count++;
    }
#if defined(XR_OS_WINDOWS)
    snprintf(out->system_libraries[out->system_library_count++], sizeof(out->system_libraries[0]),
             "%s", "ws2_32");
    snprintf(out->system_libraries[out->system_library_count++], sizeof(out->system_libraries[0]),
             "%s", "synchronization");
#else
    snprintf(out->system_libraries[out->system_library_count++], sizeof(out->system_libraries[0]),
             "%s", "m");
    snprintf(out->system_libraries[out->system_library_count++], sizeof(out->system_libraries[0]),
             "%s", "pthread");
#endif
    uint8_t digest[32];
    char identity[512];
    int written = snprintf(identity, sizeof(identity), "%s:%s:%s", target->name,
                           out->artifacts[0].sha256, out->artifacts[1].sha256);
    if (written < 0 || (size_t) written >= sizeof(identity))
        return false;
    xr_sha256((const uint8_t *) identity, (size_t) written, digest);
    char digest_hex[65];
    xtc_sha256_hex(digest, digest_hex);
    snprintf(out->sdk_digest, sizeof(out->sdk_digest), "sha256:%s", digest_hex);
    return true;
#else
    (void) target;
    (void) provider;
    (void) out;
    xtc_runtime_error(err, err_size, "Xray SDK build paths are unavailable", NULL);
    return false;
#endif
}

XR_FUNC bool xtc_runtime_manifest_load(const XrToolchainTarget *target,
                                       XrToolchainProviderId provider, const char *program_hint,
                                       XrRuntimeArtifactSet *out, XrToolchainReasonCode *reason,
                                       char *err, size_t err_size) {
    char root[1200];
    if (reason)
        *reason = XR_TOOLCHAIN_REASON_RUNTIME_ARTIFACT_MISSING;
    if (!target || !out || provider == XR_TOOLCHAIN_PROVIDER_NONE) {
        xtc_runtime_error(err, err_size, "invalid runtime artifact request", NULL);
        return false;
    }
    memset(out, 0, sizeof(*out));
    bool ok = xtc_runtime_install_root(program_hint, root, sizeof(root))
                  ? xtc_runtime_load_installed(root, target, provider, out, err, err_size)
                  : xtc_runtime_load_build_tree(target, provider, out, err, err_size);
    if (ok && reason)
        *reason = XR_TOOLCHAIN_REASON_NONE;
    return ok;
}

XR_FUNC const XrRuntimeArtifact *xtc_runtime_artifact_find(const XrRuntimeArtifactSet *set,
                                                           const char *id_prefix) {
    if (!set || !id_prefix)
        return NULL;
    size_t prefix_len = strlen(id_prefix);
    for (size_t i = 0; i < set->artifact_count; i++) {
        if (strncmp(set->artifacts[i].id, id_prefix, prefix_len) == 0)
            return &set->artifacts[i];
    }
    return NULL;
}
