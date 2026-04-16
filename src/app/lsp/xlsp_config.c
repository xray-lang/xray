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
            if (*pattern == '\0') return true;
            while (*str) {
                if (glob_match(pattern, str)) return true;
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
    while (*pattern == '*') pattern++;
    return *pattern == '\0' && *str == '\0';
}

// Add an ignore pattern to config
void xlsp_config_add_ignore(XlspConfig *config, const char *pattern, bool is_dir_only) {
    if (!config || !pattern) return;
    
    // Expand capacity if needed
    if (config->ignore_pattern_count >= config->ignore_pattern_capacity) {
        int new_cap = config->ignore_pattern_capacity == 0 ? 16 : config->ignore_pattern_capacity * 2;
        XlspIgnorePattern *new_patterns = xr_realloc(config->ignore_patterns, 
                                                   new_cap * sizeof(XlspIgnorePattern));
        if (!new_patterns) return;
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
    if (!config || !config->ignore_patterns) return;
    
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
    if (!config || !name) return false;
    
    // Always skip hidden files/dirs
    if (name[0] == '.') return true;
    
    for (int i = 0; i < config->ignore_pattern_count; i++) {
        XlspIgnorePattern *p = &config->ignore_patterns[i];
        
        // Skip file-only patterns for directories and vice versa
        if (p->is_dir_only && !is_dir) continue;
        
        // Match pattern
        if (p->is_glob) {
            if (glob_match(p->pattern, name)) return true;
        } else {
            if (strcmp(p->pattern, name) == 0) return true;
        }
    }
    
    return false;
}

// Load default ignore patterns (currently empty - user controls all ignore rules)
// Note: Hidden files/dirs (starting with '.') are always ignored in xlsp_config_should_ignore()
void xlsp_config_load_defaults(XlspConfig *config) {
    if (!config) return;
    // No default patterns - user has full control via xray.toml [lsp] section
}

// Load ignore patterns from xray.toml [lsp] section
bool xlsp_config_load_from_toml(XlspConfig *config, const char *root_path) {
    if (!config || !root_path) return false;
    
    // Build path to xray.toml
    char toml_path[XLSP_MAX_PATH];
    snprintf(toml_path, sizeof(toml_path), "%s/xray.toml", root_path);
    
    FILE *f = fopen(toml_path, "r");
    if (!f) return false;
    
    // Read file content
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = xr_malloc(size + 1);
    if (!content) {
        fclose(f);
        return false;
    }
    
    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);
    
    // Simple TOML parsing for [lsp] section
    // Look for [lsp] section
    char *lsp_section = strstr(content, "[lsp]");
    if (!lsp_section) {
        xr_free(content);
        return false;
    }
    
    // Find next section or end of file
    char *next_section = strchr(lsp_section + 5, '[');
    size_t section_len = next_section ? (size_t)(next_section - lsp_section) : strlen(lsp_section);
    
    // Look for ignore = [...] array
    char *ignore_start = strstr(lsp_section, "ignore");
    if (ignore_start && (size_t)(ignore_start - lsp_section) < section_len) {
        // Find array start
        char *array_start = strchr(ignore_start, '[');
        if (array_start && (next_section == NULL || array_start < next_section)) {
            char *array_end = strchr(array_start, ']');
            if (array_end) {
                // Parse array items
                char *p = array_start + 1;
                while (p < array_end) {
                    // Skip whitespace and commas
                    while (p < array_end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == ',')) p++;
                    if (p >= array_end) break;
                    
                    // Parse string item
                    if (*p == '"') {
                        p++;  // Skip opening quote
                        char *str_end = strchr(p, '"');
                        if (str_end && str_end < array_end) {
                            size_t len = str_end - p;
                            char *pattern = xr_malloc(len + 1);
                            memcpy(pattern, p, len);
                            pattern[len] = '\0';
                            
                            // Determine if it's a directory pattern (no extension or ends with /)
                            bool is_dir = (strchr(pattern, '.') == NULL && strchr(pattern, '*') == NULL) ||
                                         (len > 0 && pattern[len-1] == '/');
                            if (is_dir && len > 0 && pattern[len-1] == '/') {
                                pattern[len-1] = '\0';  // Remove trailing slash
                            }
                            
                            xlsp_config_add_ignore(config, pattern, is_dir);
                            xr_free(pattern);
                            
                            p = str_end + 1;
                        } else {
                            break;
                        }
                    } else {
                        p++;
                    }
                }
            }
        }
    }
    
    // Look for log_path = "..." string value
    char *log_path_start = strstr(lsp_section, "log_path");
    if (log_path_start && (size_t)(log_path_start - lsp_section) < section_len) {
        char *eq = strchr(log_path_start, '=');
        if (eq && (next_section == NULL || eq < next_section)) {
            // Skip whitespace after =
            char *p = eq + 1;
            while (*p == ' ' || *p == '\t') p++;
            
            // Parse string value
            if (*p == '"') {
                p++;
                char *str_end = strchr(p, '"');
                if (str_end) {
                    size_t len = str_end - p;
                    if (config->log_path) xr_free(config->log_path);
                    config->log_path = xr_malloc(len + 1);
                    if (config->log_path) {
                        memcpy(config->log_path, p, len);
                        config->log_path[len] = '\0';
                    }
                }
            }
        }
    }
    
    // Look for log_to_stderr = true/false
    char *log_stderr_start = strstr(lsp_section, "log_to_stderr");
    if (log_stderr_start && (size_t)(log_stderr_start - lsp_section) < section_len) {
        char *eq = strchr(log_stderr_start, '=');
        if (eq && (next_section == NULL || eq < next_section)) {
            char *p = eq + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "false", 5) == 0) {
                config->log_to_stderr = false;
            } else if (strncmp(p, "true", 4) == 0) {
                config->log_to_stderr = true;
            }
        }
    }
    
    xr_free(content);
    return true;
}
