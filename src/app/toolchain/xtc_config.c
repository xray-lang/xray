/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_config.c - User toolchain preference configuration
 */

#include "xtc_config.h"

#include "../../base/xfileio.h"
#include "../../base/xmalloc.h"
#include "../../base/xtoml.h"
#include "../../os/os_fs.h"
#include "../../os/os_proc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

typedef struct XtcConfigLock {
#ifdef XR_OS_WINDOWS
    HANDLE handle;
#else
    int fd;
#endif
} XtcConfigLock;

static void xtc_config_error(char *err, size_t err_size, const char *format, const char *arg) {
    if (!err || err_size == 0)
        return;
    if (arg)
        snprintf(err, err_size, format, arg);
    else
        snprintf(err, err_size, "%s", format);
}

static bool xtc_config_join(char *out, size_t out_size, const char *left, const char *right) {
    size_t left_len = strlen(left);
    int written = snprintf(
        out, out_size, "%s%s%s", left,
        left_len > 0 && (left[left_len - 1] == '/' || left[left_len - 1] == '\\') ? "" : "/",
        right);
    return written >= 0 && (size_t) written < out_size;
}

XR_FUNC bool xtc_config_path(char *out, size_t out_size, char *err, size_t err_size) {
#ifdef XR_OS_WINDOWS
    const char *base = getenv("LOCALAPPDATA");
    const char *suffix = "Xray/toolchains.toml";
#else
    const char *base = getenv("XDG_CONFIG_HOME");
    char fallback[1200];
    const char *suffix = "xray/toolchains.toml";
    if (!base || !base[0]) {
        const char *home = getenv("HOME");
        if (!home || !home[0] || !xtc_config_join(fallback, sizeof(fallback), home, ".config")) {
            xtc_config_error(err, err_size, "HOME/XDG_CONFIG_HOME is unavailable", NULL);
            return false;
        }
        base = fallback;
    }
#endif
    if (!base || !base[0] || !xtc_config_join(out, out_size, base, suffix)) {
        xtc_config_error(err, err_size, "toolchain config path is unavailable", NULL);
        return false;
    }
    return true;
}

XR_FUNC void xtc_config_init(XrToolchainConfig *config) {
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->schema = 1;
    config->allow_fallback = true;
    config->prefer_external_native = true;
    config->auto_probe = true;
}

static bool xtc_config_known_keys(XrTomlValue *table, const char *const *known) {
    if (!xtoml_is_table(table))
        return false;
    for (int i = 0; i < table->as.table.count; i++) {
        bool found = false;
        for (size_t k = 0; known[k]; k++) {
            if (strcmp(table->as.table.members[i].key, known[k]) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static bool xtc_config_parse_preference(XrTomlValue *table, const char *target,
                                        XrToolchainPreference *out, char *err, size_t err_size) {
    static const char *const known[] = {"provider", "compiler", "zig", NULL};
    if (!table || !xtc_config_known_keys(table, known)) {
        xtc_config_error(err, err_size, "toolchain preference has unknown or invalid fields", NULL);
        return false;
    }
    const char *provider = xtoml_get_string(table, "provider");
    if (!provider || !xtc_selector_parse(provider, &out->selector, err, err_size))
        return false;
    const char *compiler = xtoml_get_string(table, "compiler");
    const char *zig = xtoml_get_string(table, "zig");
    snprintf(out->target, sizeof(out->target), "%s", target);
    if (compiler)
        snprintf(out->compiler, sizeof(out->compiler), "%s", compiler);
    if (zig)
        snprintf(out->zig, sizeof(out->zig), "%s", zig);
    return true;
}

XR_FUNC bool xtc_config_load(const char *path, XrToolchainConfig *out, bool *exists, char *err,
                             size_t err_size) {
    if (!path || !out) {
        xtc_config_error(err, err_size, "invalid toolchain config request", NULL);
        return false;
    }
    xtc_config_init(out);
    if (exists)
        *exists = xr_fs_is_file(path);
    if (!xr_fs_is_file(path))
        return true;
    size_t size = 0;
    char *content = xr_file_read_all(path, "rb", &size);
    XrTomlValue *root = content ? xtoml_parse(content, size) : NULL;
    if (!root || !xtoml_is_table(root)) {
        xtoml_free(root);
        xr_free(content);
        xtc_config_error(err, err_size, "toolchain config is corrupt; original file was preserved",
                         NULL);
        return false;
    }
    static const char *const root_known[] = {"schema", "native", "target", "policy", NULL};
    if (!xtc_config_known_keys(root, root_known) || xtoml_get_int(root, "schema") != 1) {
        xtoml_free(root);
        xr_free(content);
        xtc_config_error(err, err_size, "unsupported or unknown toolchain config schema", NULL);
        return false;
    }
    XrTomlValue *native = xtoml_get_table(root, "native");
    if (native) {
        if (!xtc_config_parse_preference(native, "native", &out->native, err, err_size)) {
            xtoml_free(root);
            xr_free(content);
            return false;
        }
        out->has_native = true;
    }
    XrTomlValue *targets = xtoml_get_table(root, "target");
    if (targets) {
        if ((size_t) targets->as.table.count > XTC_CONFIG_MAX_TARGETS) {
            xtoml_free(root);
            xr_free(content);
            xtc_config_error(err, err_size, "too many target preferences", NULL);
            return false;
        }
        for (int i = 0; i < targets->as.table.count; i++) {
            XrTomlMember *member = &targets->as.table.members[i];
            if (!xtc_config_parse_preference(member->value, member->key,
                                             &out->targets[out->target_count], err, err_size)) {
                xtoml_free(root);
                xr_free(content);
                return false;
            }
            out->target_count++;
        }
    }
    XrTomlValue *policy = xtoml_get_table(root, "policy");
    if (policy) {
        static const char *const known[] = {"allow_fallback", "prefer_external_native",
                                            "auto_probe", NULL};
        if (!xtc_config_known_keys(policy, known)) {
            xtoml_free(root);
            xr_free(content);
            xtc_config_error(err, err_size, "toolchain policy has unknown fields", NULL);
            return false;
        }
        out->allow_fallback = xtoml_get_bool_or(policy, "allow_fallback", true);
        out->prefer_external_native = xtoml_get_bool_or(policy, "prefer_external_native", true);
        out->auto_probe = xtoml_get_bool_or(policy, "auto_probe", true);
    }
    xtoml_free(root);
    xr_free(content);
    return true;
}

XR_FUNC const XrToolchainPreference *xtc_config_find(const XrToolchainConfig *config,
                                                     const char *target) {
    if (!config || !target)
        return NULL;
    if (strcmp(target, "native") == 0)
        return config->has_native ? &config->native : NULL;
    for (size_t i = 0; i < config->target_count; i++) {
        if (strcmp(config->targets[i].target, target) == 0)
            return &config->targets[i];
    }
    return NULL;
}

XR_FUNC void xtc_config_apply_provider_paths(const XrToolchainPreference *preference,
                                             XrToolchainSelector selector, const char *requested_cc,
                                             const char *environment_cc, const char *requested_zig,
                                             const char *environment_zig, const char **out_cc,
                                             const char **out_zig) {
    if (out_cc)
        *out_cc = requested_cc;
    if (out_zig)
        *out_zig = requested_zig;
    if (!preference || preference->selector != selector)
        return;
    if (out_cc && (!requested_cc || !requested_cc[0]) && (!environment_cc || !environment_cc[0]) &&
        preference->compiler[0] && strcmp(preference->compiler, "auto") != 0)
        *out_cc = preference->compiler;
    if (out_zig && (!requested_zig || !requested_zig[0]) &&
        (!environment_zig || !environment_zig[0]) && preference->zig[0] &&
        strcmp(preference->zig, "auto") != 0 && strcmp(preference->zig, "managed") != 0)
        *out_zig = preference->zig;
}

static bool xtc_config_parent_dir(const char *path, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s", path);
    if (written < 0 || (size_t) written >= out_size)
        return false;
    char *slash = strrchr(out, '/');
    char *backslash = strrchr(out, '\\');
    char *sep = slash;
    if (backslash && (!sep || backslash > sep))
        sep = backslash;
    if (!sep || sep == out)
        return false;
    *sep = '\0';
    return true;
}

static bool xtc_config_ensure_dir(const char *path) {
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

static bool xtc_config_lock_acquire(const char *path, XtcConfigLock *lock, char *err,
                                    size_t err_size) {
    char lock_path[1300];
    int written = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    if (written < 0 || (size_t) written >= sizeof(lock_path))
        return false;
#ifdef XR_OS_WINDOWS
    lock->handle = CreateFileA(lock_path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN, NULL);
    if (lock->handle == INVALID_HANDLE_VALUE) {
        xtc_config_error(err, err_size, "cannot acquire toolchain config lock", NULL);
        return false;
    }
#else
    lock->fd = open(lock_path, O_CREAT | O_RDWR, 0600);
    if (lock->fd < 0 || flock(lock->fd, LOCK_EX) != 0) {
        if (lock->fd >= 0)
            close(lock->fd);
        xtc_config_error(err, err_size, "cannot acquire toolchain config lock", NULL);
        return false;
    }
#endif
    return true;
}

static void xtc_config_lock_release(XtcConfigLock *lock) {
#ifdef XR_OS_WINDOWS
    if (lock->handle && lock->handle != INVALID_HANDLE_VALUE)
        CloseHandle(lock->handle);
#else
    if (lock->fd >= 0) {
        (void) flock(lock->fd, LOCK_UN);
        close(lock->fd);
    }
#endif
}

static void xtc_config_write_toml_string(FILE *file, const char *text) {
    fputc('"', file);
    for (const unsigned char *p = (const unsigned char *) (text ? text : ""); *p; p++) {
        switch (*p) {
            case '"':
                fputs("\\\"", file);
                break;
            case '\\':
                fputs("\\\\", file);
                break;
            case '\b':
                fputs("\\b", file);
                break;
            case '\t':
                fputs("\\t", file);
                break;
            case '\n':
                fputs("\\n", file);
                break;
            case '\f':
                fputs("\\f", file);
                break;
            case '\r':
                fputs("\\r", file);
                break;
            default:
                if (*p < 0x20)
                    fprintf(file, "\\u%04x", (unsigned int) *p);
                else
                    fputc((int) *p, file);
                break;
        }
    }
    fputc('"', file);
}

static void xtc_config_write_preference(FILE *file, const char *header,
                                        const XrToolchainPreference *preference) {
    fprintf(file, "\n[%s]\nprovider = \"%s\"\n", header, xtc_selector_name(preference->selector));
    if (preference->compiler[0]) {
        fputs("compiler = ", file);
        xtc_config_write_toml_string(file, preference->compiler);
        fputc('\n', file);
    }
    if (preference->zig[0]) {
        fputs("zig = ", file);
        xtc_config_write_toml_string(file, preference->zig);
        fputc('\n', file);
    }
}

static bool xtc_config_write_atomic(const char *path, const XrToolchainConfig *config, char *err,
                                    size_t err_size) {
    char temp[1400];
    int written = snprintf(temp, sizeof(temp), "%s.%lld.tmp", path, (long long) xr_proc_self_pid());
    if (written < 0 || (size_t) written >= sizeof(temp))
        return false;
    FILE *file = fopen(temp, "wb");
    if (!file) {
        xtc_config_error(err, err_size, "cannot create temporary toolchain config", NULL);
        return false;
    }
    fprintf(file, "schema = 1\n");
    if (config->has_native)
        xtc_config_write_preference(file, "native", &config->native);
    for (size_t i = 0; i < config->target_count; i++) {
        char header[180];
        snprintf(header, sizeof(header), "target.\"%s\"", config->targets[i].target);
        xtc_config_write_preference(file, header, &config->targets[i]);
    }
    fprintf(file, "\n[policy]\nallow_fallback = %s\nprefer_external_native = %s\nauto_probe = %s\n",
            config->allow_fallback ? "true" : "false",
            config->prefer_external_native ? "true" : "false",
            config->auto_probe ? "true" : "false");
    bool ok = fflush(file) == 0;
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
        xtc_config_error(err, err_size, "cannot atomically update toolchain config", NULL);
        return false;
    }
    return true;
}

static XrToolchainPreference *xtc_config_find_mut(XrToolchainConfig *config, const char *target,
                                                  bool create) {
    if (strcmp(target, "native") == 0) {
        if (create)
            config->has_native = true;
        return config->has_native ? &config->native : NULL;
    }
    for (size_t i = 0; i < config->target_count; i++) {
        if (strcmp(config->targets[i].target, target) == 0)
            return &config->targets[i];
    }
    if (!create || config->target_count >= XTC_CONFIG_MAX_TARGETS)
        return NULL;
    XrToolchainPreference *preference = &config->targets[config->target_count++];
    memset(preference, 0, sizeof(*preference));
    snprintf(preference->target, sizeof(preference->target), "%s", target);
    return preference;
}

XR_FUNC bool xtc_config_use(const char *path, const char *target, XrToolchainSelector selector,
                            const char *cc, const char *zig, char *err, size_t err_size) {
    char dir[1200];
    if (!path || !target || !xtc_config_parent_dir(path, dir, sizeof(dir)) ||
        !xtc_config_ensure_dir(dir)) {
        xtc_config_error(err, err_size, "cannot create toolchain config directory", NULL);
        return false;
    }
    XtcConfigLock lock = {0};
#ifndef XR_OS_WINDOWS
    lock.fd = -1;
#endif
    if (!xtc_config_lock_acquire(path, &lock, err, err_size))
        return false;
    XrToolchainConfig config;
    bool exists;
    bool ok = xtc_config_load(path, &config, &exists, err, err_size);
    if (ok) {
        XrToolchainPreference *preference = xtc_config_find_mut(&config, target, true);
        if (!preference) {
            xtc_config_error(err, err_size, "too many target preferences", NULL);
            ok = false;
        } else {
            snprintf(preference->target, sizeof(preference->target), "%s", target);
            preference->selector = selector;
            preference->compiler[0] = '\0';
            preference->zig[0] = '\0';
            if (cc)
                snprintf(preference->compiler, sizeof(preference->compiler), "%s", cc);
            if (zig)
                snprintf(preference->zig, sizeof(preference->zig), "%s", zig);
            ok = xtc_config_write_atomic(path, &config, err, err_size);
        }
    }
    xtc_config_lock_release(&lock);
    return ok;
}

XR_FUNC bool xtc_config_reset(const char *path, const char *target, char *err, size_t err_size) {
    if (!path)
        return false;
    if (!xr_fs_is_file(path))
        return true;
    char dir[1200];
    if (!xtc_config_parent_dir(path, dir, sizeof(dir)))
        return false;
    XtcConfigLock lock = {0};
#ifndef XR_OS_WINDOWS
    lock.fd = -1;
#endif
    if (!xtc_config_lock_acquire(path, &lock, err, err_size))
        return false;
    bool ok = true;
    if (!target) {
        ok = xr_fs_remove(path) == 0;
        if (!ok)
            xtc_config_error(err, err_size, "cannot remove toolchain config", NULL);
    } else {
        XrToolchainConfig config;
        bool exists;
        ok = xtc_config_load(path, &config, &exists, err, err_size);
        if (ok && strcmp(target, "native") == 0) {
            config.has_native = false;
            memset(&config.native, 0, sizeof(config.native));
        } else if (ok) {
            for (size_t i = 0; i < config.target_count; i++) {
                if (strcmp(config.targets[i].target, target) != 0)
                    continue;
                for (size_t j = i + 1; j < config.target_count; j++)
                    config.targets[j - 1] = config.targets[j];
                config.target_count--;
                break;
            }
        }
        if (ok)
            ok = xtc_config_write_atomic(path, &config, err, err_size);
    }
    xtc_config_lock_release(&lock);
    return ok;
}
