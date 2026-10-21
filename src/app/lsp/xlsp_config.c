/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_config.c - LSP configuration and ignore pattern management
 *
 * KEY CONCEPT:
 *   Handles loading configuration from xray.toml [lsp] section
 *   and managing workspace ignore patterns for file scanning.
 */

#include "../../base/xresource_limits.h"
#include "xlsp_source_config.h"
#include "../../api/xsource_compile.h"
#include "../../base/xmalloc.h"
#include <limits.h>
#include <string.h>

// Check if pattern contains glob wildcards
static bool pattern_is_glob(const char *pattern) {
    return strchr(pattern, '*') != NULL || strchr(pattern, '?') != NULL;
}

XR_FUNC void xlsp_config_add_ignore(XlspConfig *config, const char *pattern, bool is_dir_only) {
    if (!config || !pattern || config->ignore_pattern_count < 0 ||
        config->ignore_pattern_capacity < config->ignore_pattern_count ||
        (config->ignore_pattern_count && !config->ignore_patterns) ||
        config->ignore_pattern_count == INT_MAX)
        return;
    char *copy = xr_strdup(pattern);
    if (!copy) return;
    if (config->ignore_pattern_count == config->ignore_pattern_capacity) {
        int capacity = config->ignore_pattern_capacity;
        int new_capacity = !capacity ? 16 : capacity > INT_MAX / 2 ? INT_MAX : capacity * 2;
        if ((size_t)new_capacity > SIZE_MAX / sizeof(XlspIgnorePattern)) {
            xr_free(copy); return;
        }
        XlspIgnorePattern *patterns = xr_realloc(config->ignore_patterns,
            (size_t)new_capacity * sizeof(*patterns));
        if (!patterns) { xr_free(copy); return; }
        config->ignore_patterns = patterns;
        config->ignore_pattern_capacity = new_capacity;
    }
    config->ignore_patterns[config->ignore_pattern_count++] =
        (XlspIgnorePattern){copy, pattern_is_glob(pattern), is_dir_only};
}

XR_FUNC void xlsp_config_free_ignores(XlspConfig *config) {
    if (!config) return;
    for (int i = 0; i < config->ignore_pattern_count; ++i)
        xr_free(config->ignore_patterns[i].pattern);
    xr_free(config->ignore_patterns);
    config->ignore_patterns = NULL;
    config->ignore_pattern_count = config->ignore_pattern_capacity = 0;
}

// Hidden names are handled by the shared ignore matcher; other rules are explicit.
XR_FUNC void xlsp_config_load_defaults(XlspConfig *config) {
    (void)config;
}

XR_FUNC bool xlsp_config_load_from_toml(XlspConfig *config, const char *root_path) {
    if (!config || !root_path) return false;
    XrCompileResourceLimits resources_limits = (XrCompileResourceLimits)XR_RESOURCE_SANDBOX_COMPILER_LIMITS;
    XrTomlParseLimits parse_limits = xr_source_compile_default_manifest_limits();
    XrCompileResources *resources = NULL;
    if (xr_compile_resources_new(&resources_limits, &resources) != XR_COMPILE_RESOURCE_OK)
        return false;
    XlspSourceConfig *owner = NULL;
    XrXirStatus status = xlsp_source_config_read(resources, root_path, config, &parse_limits, &owner);
    if (status == XR_XIR_OK) status = xlsp_source_config_apply(owner, config);
    xlsp_source_config_free(owner);
    xr_compile_resources_release(resources);
    return status == XR_XIR_OK;
}
