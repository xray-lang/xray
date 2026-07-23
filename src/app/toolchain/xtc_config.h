/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_config.h - User toolchain preference configuration
 */

#ifndef XTC_CONFIG_H
#define XTC_CONFIG_H

#include "xtc_model.h"

#define XTC_CONFIG_MAX_TARGETS 16

typedef struct XrToolchainPreference {
    char target[128];
    XrToolchainSelector selector;
    char compiler[1200];
    char zig[1200];
} XrToolchainPreference;

typedef struct XrToolchainConfig {
    int schema;
    bool has_native;
    XrToolchainPreference native;
    XrToolchainPreference targets[XTC_CONFIG_MAX_TARGETS];
    size_t target_count;
    bool allow_fallback;
    bool prefer_external_native;
    bool auto_probe;
} XrToolchainConfig;

XR_FUNC bool xtc_config_path(char *out, size_t out_size, char *err, size_t err_size);
XR_FUNC void xtc_config_init(XrToolchainConfig *config);
XR_FUNC bool xtc_config_load(const char *path, XrToolchainConfig *out, bool *exists, char *err,
                             size_t err_size);
XR_FUNC const XrToolchainPreference *xtc_config_find(const XrToolchainConfig *config,
                                                     const char *target);
XR_FUNC bool xtc_config_use(const char *path, const char *target, XrToolchainSelector selector,
                            const char *cc, const char *zig, char *err, size_t err_size);
XR_FUNC bool xtc_config_reset(const char *path, const char *target, char *err, size_t err_size);

#endif /* XTC_CONFIG_H */
