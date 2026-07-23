/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_probe_cache.c - Fingerprint-validated successful probe cache
 */

#include "xtc_probe_cache.h"

#include "../../base/xfileio.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../os/os_dir.h"
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

static void xtc_cache_error(char *err, size_t err_size, const char *message) {
    if (err && err_size)
        snprintf(err, err_size, "%s", message);
}

static const char *xtc_cache_profile_name(XrToolchainProfile profile) {
    return profile == XR_TOOLCHAIN_PROFILE_FREESTANDING ? "freestanding" : "hosted";
}

static bool xtc_cache_join(char *out, size_t out_size, const char *left, const char *right) {
    size_t len = strlen(left);
    int written =
        snprintf(out, out_size, "%s%s%s", left,
                 len && (left[len - 1] == '/' || left[len - 1] == '\\') ? "" : "/", right);
    return written >= 0 && (size_t) written < out_size;
}

static bool xtc_cache_ensure_dir(const char *path) {
    char buffer[1200];
    int written = snprintf(buffer, sizeof(buffer), "%s", path);
    if (written < 0 || (size_t) written >= sizeof(buffer))
        return false;
    for (char *p = buffer + 1; *p; p++) {
        if (*p != '/' && *p != '\\')
            continue;
        char saved = *p;
        *p = '\0';
        if (!xr_fs_is_dir(buffer) && xr_fs_mkdir(buffer, 0700) != 0)
            return false;
        *p = saved;
    }
    return xr_fs_is_dir(buffer) || xr_fs_mkdir(buffer, 0700) == 0;
}

static bool xtc_cache_dir(char *out, size_t out_size, char *err, size_t err_size) {
#ifdef XR_OS_WINDOWS
    const char *base = getenv("LOCALAPPDATA");
    const char *suffix = "Xray/cache/toolchain/probes";
#else
    const char *base = getenv("XDG_CACHE_HOME");
    char fallback[1200];
    const char *suffix = "xray/toolchain/probes";
    if (!base || !base[0]) {
        const char *home = getenv("HOME");
        if (!home || !home[0] || !xtc_cache_join(fallback, sizeof(fallback), home, ".cache")) {
            xtc_cache_error(err, err_size, "HOME/XDG_CACHE_HOME is unavailable");
            return false;
        }
        base = fallback;
    }
#endif
    if (!base || !base[0] || !xtc_cache_join(out, out_size, base, suffix)) {
        xtc_cache_error(err, err_size, "toolchain probe cache path is unavailable");
        return false;
    }
    return true;
}

static bool xtc_cache_file(const XrToolchainProbeOptions *options, char *out, size_t out_size,
                           char *err, size_t err_size) {
    char dir[1200];
    char name[320];
    if (!xtc_cache_dir(dir, sizeof(dir), err, err_size))
        return false;
    int written = snprintf(name, sizeof(name), "%s.%s.%s.json", options->request.target.name,
                           xtc_cache_profile_name(options->profile),
                           xtc_selector_name(options->request.selector));
    return written >= 0 && (size_t) written < sizeof(name) &&
           xtc_cache_join(out, out_size, dir, name);
}

static bool xtc_cache_capability(int64_t value, XrToolchainCapabilityState *out) {
    if (value < XR_TOOLCHAIN_CAPABILITY_UNSUPPORTED || value > XR_TOOLCHAIN_CAPABILITY_OK)
        return false;
    *out = (XrToolchainCapabilityState) value;
    return true;
}

XR_FUNC bool xtc_probe_cache_load(const XrToolchainProbeOptions *options,
                                  XrToolchainProbeResult *result, bool *hit, char *err,
                                  size_t err_size) {
    char path[1400];
    if (hit)
        *hit = false;
    if (!options || !result || !xtc_cache_file(options, path, sizeof(path), err, err_size))
        return false;
    if (!xr_fs_is_file(path))
        return true;
    size_t size = 0;
    char *json = xr_file_read_all(path, "rb", &size);
    XrJsonValue *doc = json ? xjson_parse(json, size) : NULL;
    const char *fingerprint = doc ? xjson_get_string(doc, "fingerprint") : NULL;
    const char *provider = doc ? xjson_get_string(doc, "provider") : NULL;
    const char *compiler_fingerprint = doc ? xjson_get_string(doc, "compilerFingerprint") : NULL;
    const char *runtime_artifact = doc ? xjson_get_string(doc, "runtimeArtifact") : NULL;
    bool valid = doc && xjson_is_object(doc) && xjson_get_int(doc, "schema") == 1 && fingerprint &&
                 strcmp(fingerprint, result->selection.probe_fingerprint) == 0 && provider &&
                 strcmp(provider, xtc_provider_name(result->selection.provider)) == 0 &&
                 compiler_fingerprint && runtime_artifact &&
                 strcmp(runtime_artifact, result->selection.runtime_artifact) == 0 &&
                 xjson_get_bool(doc, "ready");
    XrToolchainCapabilityState c_compile, sdk_compile, runtime_link, native_run, cross, lto;
    valid = valid && xtc_cache_capability(xjson_get_int(doc, "cCompile"), &c_compile) &&
            xtc_cache_capability(xjson_get_int(doc, "sdkCompile"), &sdk_compile) &&
            xtc_cache_capability(xjson_get_int(doc, "runtimeLink"), &runtime_link) &&
            xtc_cache_capability(xjson_get_int(doc, "nativeRun"), &native_run) &&
            xtc_cache_capability(xjson_get_int(doc, "cross"), &cross) &&
            xtc_cache_capability(xjson_get_int(doc, "lto"), &lto);
    if (valid) {
        snprintf(result->selection.compiler_fingerprint,
                 sizeof(result->selection.compiler_fingerprint), "%s", compiler_fingerprint);
        result->selection.readiness = XR_TOOLCHAIN_READY;
        result->selection.reason = XR_TOOLCHAIN_REASON_NONE;
        result->c_compile = c_compile;
        result->sdk_compile = sdk_compile;
        result->runtime_link = runtime_link;
        result->native_run = native_run;
        result->cross = cross;
        result->lto = lto;
        snprintf(result->cache, sizeof(result->cache), "%s", "hit");
        if (hit)
            *hit = true;
    }
    xjson_free(doc);
    xr_free(json);
    return true;
}

static bool xtc_cache_write_atomic(const char *path, const char *data, size_t size, char *err,
                                   size_t err_size) {
    char temp[1500];
    int written = snprintf(temp, sizeof(temp), "%s.%lld.tmp", path, (long long) xr_proc_self_pid());
    if (written < 0 || (size_t) written >= sizeof(temp))
        return false;
    FILE *file = fopen(temp, "wb");
    if (!file) {
        xtc_cache_error(err, err_size, "cannot create temporary probe cache");
        return false;
    }
    bool ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
#ifdef XR_OS_WINDOWS
    if (ok)
        ok = _commit(_fileno(file)) == 0;
#else
    if (ok)
        ok = fsync(fileno(file)) == 0;
#endif
    if (fclose(file) != 0)
        ok = false;
    if (!ok || xr_fs_rename(temp, path) != 0) {
        (void) xr_fs_remove(temp);
        xtc_cache_error(err, err_size, "cannot atomically update probe cache");
        return false;
    }
    return true;
}

XR_FUNC bool xtc_probe_cache_store(const XrToolchainProbeOptions *options,
                                   const XrToolchainProbeResult *result, char *err,
                                   size_t err_size) {
    char path[1400];
    char dir[1200];
    if (!options || !result || result->selection.readiness != XR_TOOLCHAIN_READY ||
        !xtc_cache_file(options, path, sizeof(path), err, err_size) ||
        !xtc_cache_dir(dir, sizeof(dir), err, err_size) || !xtc_cache_ensure_dir(dir)) {
        xtc_cache_error(err, err_size, "cannot prepare toolchain probe cache directory");
        return false;
    }
    XrJsonValue *doc = xjson_new_object();
    XJSON_SET_INT(doc, "schema", 1);
    XJSON_SET_STRING(doc, "fingerprint", result->selection.probe_fingerprint);
    XJSON_SET_STRING(doc, "provider", xtc_provider_name(result->selection.provider));
    XJSON_SET_STRING(doc, "compilerFingerprint", result->selection.compiler_fingerprint);
    XJSON_SET_STRING(doc, "runtimeArtifact", result->selection.runtime_artifact);
    XJSON_SET_BOOL(doc, "ready", true);
    XJSON_SET_INT(doc, "cCompile", result->c_compile);
    XJSON_SET_INT(doc, "sdkCompile", result->sdk_compile);
    XJSON_SET_INT(doc, "runtimeLink", result->runtime_link);
    XJSON_SET_INT(doc, "nativeRun", result->native_run);
    XJSON_SET_INT(doc, "cross", result->cross);
    XJSON_SET_INT(doc, "lto", result->lto);
    size_t size = 0;
    char *json = xjson_stringify(doc, &size);
    bool ok = json && xtc_cache_write_atomic(path, json, size, err, err_size);
    xr_free(json);
    xjson_free(doc);
    return ok;
}

XR_FUNC bool xtc_probe_cache_reset(const char *normalized_target, char *err, size_t err_size) {
    char dir[1200];
    if (!xtc_cache_dir(dir, sizeof(dir), err, err_size))
        return false;
    if (!xr_fs_is_dir(dir))
        return true;
    XrDirIter *iter = xr_dir_open(dir);
    if (!iter) {
        xtc_cache_error(err, err_size, "cannot open toolchain probe cache");
        return false;
    }
    bool ok = true;
    size_t target_len = normalized_target ? strlen(normalized_target) : 0;
    XrDirEntry entry;
    while (xr_dir_next(iter, &entry)) {
        if (entry.is_dir || !strstr(entry.name, ".json"))
            continue;
        if (normalized_target && (strncmp(entry.name, normalized_target, target_len) != 0 ||
                                  entry.name[target_len] != '.'))
            continue;
        char path[1400];
        if (!xtc_cache_join(path, sizeof(path), dir, entry.name) || xr_fs_remove(path) != 0)
            ok = false;
    }
    xr_dir_close(iter);
    if (!ok)
        xtc_cache_error(err, err_size, "cannot remove one or more probe cache entries");
    return ok;
}

XR_FUNC bool xtc_probe_cache_list(const char *normalized_target,
                                  XrToolchainProbeCacheEntry *entries, size_t capacity,
                                  size_t *out_count, char *err, size_t err_size) {
    char dir[1200];
    if (out_count)
        *out_count = 0;
    if ((!entries && capacity > 0) || !out_count || !xtc_cache_dir(dir, sizeof(dir), err, err_size))
        return false;
    if (!xr_fs_is_dir(dir))
        return true;
    XrDirIter *iter = xr_dir_open(dir);
    if (!iter) {
        xtc_cache_error(err, err_size, "cannot open toolchain probe cache");
        return false;
    }
    size_t target_len = normalized_target ? strlen(normalized_target) : 0;
    XrDirEntry entry;
    while (*out_count < capacity && xr_dir_next(iter, &entry)) {
        if (entry.is_dir || !strstr(entry.name, ".json"))
            continue;
        if (normalized_target && (strncmp(entry.name, normalized_target, target_len) != 0 ||
                                  entry.name[target_len] != '.'))
            continue;
        char path[1400];
        if (!xtc_cache_join(path, sizeof(path), dir, entry.name))
            continue;
        size_t size = 0;
        char *json = xr_file_read_all(path, "rb", &size);
        XrJsonValue *doc = json ? xjson_parse(json, size) : NULL;
        const char *provider = doc ? xjson_get_string(doc, "provider") : NULL;
        const char *fingerprint = doc ? xjson_get_string(doc, "fingerprint") : NULL;
        const char *runtime = doc ? xjson_get_string(doc, "runtimeArtifact") : NULL;
        if (doc && xjson_is_object(doc) && xjson_get_int(doc, "schema") == 1 && provider &&
            fingerprint && runtime) {
            XrToolchainProbeCacheEntry *item = &entries[(*out_count)++];
            memset(item, 0, sizeof(*item));
            snprintf(item->key, sizeof(item->key), "%s", entry.name);
            snprintf(item->provider, sizeof(item->provider), "%s", provider);
            snprintf(item->fingerprint, sizeof(item->fingerprint), "%s", fingerprint);
            snprintf(item->runtime_artifact, sizeof(item->runtime_artifact), "%s", runtime);
            item->ready = xjson_get_bool(doc, "ready");
        }
        xjson_free(doc);
        xr_free(json);
    }
    xr_dir_close(iter);
    return true;
}
