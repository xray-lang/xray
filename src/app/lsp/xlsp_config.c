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

#include "xlsp_server.h"
#include "xlsp_utils.h"
#include "../../base/xmalloc.h"
#include "../../base/xtoml.h"
#include "../../base/xfileio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Check if pattern contains glob wildcards
static bool pattern_is_glob(const char *pattern) {
    return strchr(pattern, '*') != NULL || strchr(pattern, '?') != NULL;
}

// Simple glob matching (supports * and ?)
static bool glob_match(const char *pattern, const char *str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0')
                return true;
            while (*str) {
                if (glob_match(pattern, str))
                    return true;
                str++;
            }
            return false;
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return false;
        }
    }
    while (*pattern == '*')
        pattern++;
    return *pattern == '\0' && *str == '\0';
}

// Add an ignore pattern to config
void xlsp_config_add_ignore(XlspConfig *config, const char *pattern, bool is_dir_only) {
    if (!config || !pattern)
        return;

    // Expand capacity if needed
    if (config->ignore_pattern_count >= config->ignore_pattern_capacity) {
        int new_cap =
            config->ignore_pattern_capacity == 0 ? 16 : config->ignore_pattern_capacity * 2;
        XlspIgnorePattern *new_patterns =
            xr_realloc(config->ignore_patterns, new_cap * sizeof(XlspIgnorePattern));
        if (!new_patterns)
            return;
        config->ignore_patterns = new_patterns;
        config->ignore_pattern_capacity = new_cap;
    }

    XlspIgnorePattern *p = &config->ignore_patterns[config->ignore_pattern_count++];
    p->pattern = xr_strdup(pattern);
    p->is_glob = pattern_is_glob(pattern);
    p->is_dir_only = is_dir_only;
}

// Free all ignore patterns
void xlsp_config_free_ignores(XlspConfig *config) {
    if (!config || !config->ignore_patterns)
        return;

    for (int i = 0; i < config->ignore_pattern_count; i++) {
        xr_free(config->ignore_patterns[i].pattern);
    }
    xr_free(config->ignore_patterns);
    config->ignore_patterns = NULL;
    config->ignore_pattern_count = 0;
    config->ignore_pattern_capacity = 0;
}

// Check if a name should be ignored
bool xlsp_config_should_ignore(XlspConfig *config, const char *name, bool is_dir) {
    if (!config || !name)
        return false;

    // Always skip hidden files/dirs
    if (name[0] == '.')
        return true;

    for (int i = 0; i < config->ignore_pattern_count; i++) {
        XlspIgnorePattern *p = &config->ignore_patterns[i];

        // Skip file-only patterns for directories and vice versa
        if (p->is_dir_only && !is_dir)
            continue;

        // Match pattern
        if (p->is_glob) {
            if (glob_match(p->pattern, name))
                return true;
        } else {
            if (strcmp(p->pattern, name) == 0)
                return true;
        }
    }

    return false;
}

// Load default ignore patterns (currently empty - user controls all ignore rules)
// Note: Hidden files/dirs (starting with '.') are always ignored in xlsp_config_should_ignore()
void xlsp_config_load_defaults(XlspConfig *config) {
    if (!config)
        return;
    // No default patterns - user has full control via xray.toml [lsp] section
}

// ============================================================================

// Load configuration from xray.toml [lsp] section
bool xlsp_config_load_from_toml(XlspConfig *config, const char *root_path) {
    if (!config || !root_path)
        return false;

    // Build path to xray.toml
    char toml_path[XLSP_MAX_PATH];
    snprintf(toml_path, sizeof(toml_path), "%s/xray.toml", root_path);

    size_t content_size;
    char *content = xr_file_read_all(toml_path, "r", &content_size);
    if (!content)
        return false;

    XrTomlValue *root = xtoml_parse(content, content_size);
    xr_free(content);
    if (!root)
        return false;

    XrTomlValue *lsp = xtoml_get_table(root, "lsp");
    if (!lsp) {
        xtoml_free(root);
        return false;
    }

    // --- ignore = [...] array ---
    XrTomlValue *ignore_arr = xtoml_get_array(lsp, "ignore");
    if (ignore_arr) {
        int n = xtoml_array_len(ignore_arr);
        for (int i = 0; i < n; i++) {
            XrTomlValue *elem = xtoml_array_get(ignore_arr, i);
            if (!elem || !xtoml_is_string(elem))
                continue;
            const char *pat = elem->as.string;
            size_t len = strlen(pat);
            if (len == 0)
                continue;

            // Determine if pattern is directory-only
            bool is_dir =
                (strchr(pat, '.') == NULL && strchr(pat, '*') == NULL) || (pat[len - 1] == '/');

            if (is_dir && pat[len - 1] == '/') {
                // Strip trailing '/'
                char *trimmed = xr_strdup(pat);
                if (trimmed) {
                    trimmed[len - 1] = '\0';
                    xlsp_config_add_ignore(config, trimmed, true);
                    xr_free(trimmed);
                }
            } else {
                xlsp_config_add_ignore(config, pat, is_dir);
            }
        }
    }

    // --- Scalar config values ---
    const char *sval;

    // Logging
    sval = xtoml_get_string(lsp, "log_path");
    if (sval) {
        if (config->log_path)
            xr_free(config->log_path);
        config->log_path = xr_strdup(sval);
    }
    XrTomlValue *v;
    v = xtoml_get(lsp, "log_to_stderr");
    if (v && xtoml_is_bool(v))
        config->log_to_stderr = v->as.boolean;

    // Diagnostics
    v = xtoml_get(lsp, "diagnostics_enabled");
    if (v && xtoml_is_bool(v))
        config->diagnostics_enabled = v->as.boolean;
    v = xtoml_get(lsp, "diagnostic_debounce_ms");
    if (v && xtoml_is_integer(v))
        config->diagnostic_debounce_ms = (int) v->as.integer;

    // Completion
    v = xtoml_get(lsp, "completion_max_items");
    if (v && xtoml_is_integer(v))
        config->completion_max_items = (int) v->as.integer;

    // Formatting
    v = xtoml_get(lsp, "format_tab_size");
    if (v && xtoml_is_integer(v))
        config->format_tab_size = (int) v->as.integer;
    v = xtoml_get(lsp, "format_insert_spaces");
    if (v && xtoml_is_bool(v))
        config->format_insert_spaces = v->as.boolean;
    v = xtoml_get(lsp, "format_align_match_arms");
    if (v && xtoml_is_bool(v))
        config->format_align_match_arms = v->as.boolean;
    v = xtoml_get(lsp, "format_max_line_length");
    if (v && xtoml_is_integer(v))
        config->format_max_line_length = (int) v->as.integer;
    v = xtoml_get(lsp, "format_align_enum_values");
    if (v && xtoml_is_bool(v))
        config->format_align_enum_values = v->as.boolean;
    v = xtoml_get(lsp, "format_align_struct_fields");
    if (v && xtoml_is_bool(v))
        config->format_align_struct_fields = v->as.boolean;
    v = xtoml_get(lsp, "format_align_trailing_comments");
    if (v && xtoml_is_bool(v))
        config->format_align_trailing_comments = v->as.boolean;
    v = xtoml_get(lsp, "format_wrap_long_lines");
    if (v && xtoml_is_bool(v))
        config->format_wrap_long_lines = v->as.boolean;
    v = xtoml_get(lsp, "format_multiline_trailing_comma");
    if (v && xtoml_is_bool(v))
        config->format_multiline_trailing_comma = v->as.boolean;

    // Inlay hints
    v = xtoml_get(lsp, "inlay_hints_type_annotations");
    if (v && xtoml_is_bool(v))
        config->inlay_hints_type_annotations = v->as.boolean;
    v = xtoml_get(lsp, "inlay_hints_parameter_names");
    if (v && xtoml_is_bool(v))
        config->inlay_hints_parameter_names = v->as.boolean;

    xtoml_free(root);
    return true;
}
