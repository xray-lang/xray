/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lsp_config.c - Unit tests for LSP configuration and ignore patterns
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../../src/app/lsp/xlsp_server.h"
#include "../../../src/base/xmalloc.h"
#include "../test_win_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Testing %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASS\n");                                                                          \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL at line %d: %s\n", __LINE__, #cond);                                      \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

// ============================================================================
// Default Configuration Tests
// ============================================================================

TEST(config_defaults) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    ASSERT_EQ(server->config.diagnostic_debounce_ms, 300);
    ASSERT_EQ(server->config.diagnostics_enabled, true);
    ASSERT_EQ(server->config.completion_max_items, 100);
    ASSERT_EQ(server->config.format_tab_size, 4);
    ASSERT_EQ(server->config.format_insert_spaces, true);
    ASSERT_EQ(server->config.inlay_hints_type_annotations, true);
    ASSERT_EQ(server->config.inlay_hints_parameter_names, true);
    ASSERT_EQ(server->config.log_to_stderr, true);

    xlsp_server_free(server);
}

// ============================================================================
// Ignore Pattern Tests
// ============================================================================

TEST(ignore_add_pattern) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    xlsp_config_add_ignore(&config, "node_modules", true);
    ASSERT_EQ(config.ignore_pattern_count, 1);
    ASSERT_STR_EQ(config.ignore_patterns[0].pattern, "node_modules");
    ASSERT_EQ(config.ignore_patterns[0].is_dir_only, true);
    ASSERT_EQ(config.ignore_patterns[0].is_glob, false);

    xlsp_config_free_ignores(&config);
    ASSERT_EQ(config.ignore_pattern_count, 0);
    ASSERT(config.ignore_patterns == NULL);
}

TEST(ignore_glob_pattern) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    xlsp_config_add_ignore(&config, "*.log", false);
    ASSERT_EQ(config.ignore_pattern_count, 1);
    ASSERT_EQ(config.ignore_patterns[0].is_glob, true);

    xlsp_config_add_ignore(&config, "temp_?", false);
    ASSERT_EQ(config.ignore_pattern_count, 2);
    ASSERT_EQ(config.ignore_patterns[1].is_glob, true);

    xlsp_config_free_ignores(&config);
}

TEST(ignore_should_ignore_hidden) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    // Hidden files are always ignored regardless of patterns
    ASSERT_EQ(xlsp_config_should_ignore(&config, ".git", true), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, ".hidden_file", false), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "..", true), true);

    // Non-hidden should not be ignored without patterns
    ASSERT_EQ(xlsp_config_should_ignore(&config, "src", true), false);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "main.xr", false), false);

    xlsp_config_free_ignores(&config);
}

TEST(ignore_exact_match) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    xlsp_config_add_ignore(&config, "node_modules", true);

    // Exact match on directory
    ASSERT_EQ(xlsp_config_should_ignore(&config, "node_modules", true), true);
    // Dir-only pattern should not match files
    ASSERT_EQ(xlsp_config_should_ignore(&config, "node_modules", false), false);
    // Different name should not match
    ASSERT_EQ(xlsp_config_should_ignore(&config, "src", true), false);

    xlsp_config_free_ignores(&config);
}

TEST(ignore_glob_match) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    xlsp_config_add_ignore(&config, "*.log", false);

    ASSERT_EQ(xlsp_config_should_ignore(&config, "error.log", false), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "debug.log", false), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "main.xr", false), false);

    xlsp_config_free_ignores(&config);
}

TEST(ignore_multiple_patterns) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    xlsp_config_add_ignore(&config, "node_modules", true);
    xlsp_config_add_ignore(&config, "build", true);
    xlsp_config_add_ignore(&config, "*.o", false);
    xlsp_config_add_ignore(&config, "*.tmp", false);

    ASSERT_EQ(config.ignore_pattern_count, 4);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "node_modules", true), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "build", true), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "main.o", false), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "cache.tmp", false), true);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "src", true), false);
    ASSERT_EQ(xlsp_config_should_ignore(&config, "main.xr", false), false);

    xlsp_config_free_ignores(&config);
}

TEST(ignore_capacity_growth) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    // Add more patterns than initial capacity (16)
    for (int i = 0; i < 20; i++) {
        char pat[32];
        snprintf(pat, sizeof(pat), "pattern_%d", i);
        xlsp_config_add_ignore(&config, pat, false);
    }

    ASSERT_EQ(config.ignore_pattern_count, 20);
    ASSERT(config.ignore_pattern_capacity >= 20);

    // Verify first and last patterns survive realloc
    ASSERT_STR_EQ(config.ignore_patterns[0].pattern, "pattern_0");
    ASSERT_STR_EQ(config.ignore_patterns[19].pattern, "pattern_19");

    xlsp_config_free_ignores(&config);
}

TEST(ignore_null_safety) {
    // NULL config should not crash
    xlsp_config_add_ignore(NULL, "test", false);
    xlsp_config_should_ignore(NULL, "test", false);
    xlsp_config_free_ignores(NULL);

    XlspConfig config;
    memset(&config, 0, sizeof(config));

    // NULL pattern should not crash
    xlsp_config_add_ignore(&config, NULL, false);
    ASSERT_EQ(config.ignore_pattern_count, 0);

    // NULL name should not crash
    ASSERT_EQ(xlsp_config_should_ignore(&config, NULL, false), false);

    xlsp_config_free_ignores(&config);
}

// ============================================================================
// TOML Loading Tests
// ============================================================================

// Helper: create a temporary directory with a xray.toml file
static char *create_temp_toml(const char *content) {
    char template[] = "/tmp/xray_test_XXXXXX";
    char *tmpdir = mkdtemp(template);
    if (!tmpdir)
        return NULL;

    char *dir = xr_strdup(tmpdir);
    if (!dir)
        return NULL;

    char path[256];
    snprintf(path, sizeof(path), "%s/xray.toml", dir);

    FILE *f = fopen(path, "w");
    if (!f) {
        xr_free(dir);
        return NULL;
    }
    fputs(content, f);
    fclose(f);

    return dir;
}

static void cleanup_temp_toml(char *dir) {
    if (!dir)
        return;
    char path[256];
    snprintf(path, sizeof(path), "%s/xray.toml", dir);
    unlink(path);
    rmdir(dir);
    xr_free(dir);
}

TEST(toml_load_basic) {
    char *dir = create_temp_toml("[lsp]\n"
                                 "diagnostics_enabled = false\n"
                                 "diagnostic_debounce_ms = 500\n"
                                 "completion_max_items = 50\n"
                                 "format_tab_size = 2\n"
                                 "format_insert_spaces = false\n");
    ASSERT(dir != NULL);

    XlspConfig config;
    memset(&config, 0, sizeof(config));
    // Set defaults to verify override
    config.diagnostics_enabled = true;
    config.diagnostic_debounce_ms = 300;
    config.completion_max_items = 100;
    config.format_tab_size = 4;
    config.format_insert_spaces = true;

    bool ok = xlsp_config_load_from_toml(&config, dir);
    ASSERT(ok);

    ASSERT_EQ(config.diagnostics_enabled, false);
    ASSERT_EQ(config.diagnostic_debounce_ms, 500);
    ASSERT_EQ(config.completion_max_items, 50);
    ASSERT_EQ(config.format_tab_size, 2);
    ASSERT_EQ(config.format_insert_spaces, false);

    xlsp_config_free_ignores(&config);
    cleanup_temp_toml(dir);
}

TEST(toml_load_ignore_array) {
    // Known limitation: the simple TOML parser uses strchr('[') to find the
    // next section boundary, which conflicts with array value brackets.
    // When [lsp] is the LAST section, there is no next '[' section marker,
    // so the parser falls back to strlen() and correctly parses the array.
    char *dir = create_temp_toml("[lsp]\n"
                                 "diagnostics_enabled = true\n"
                                 "ignore = [\"node_modules\", \"build\", \"*.log\"]\n");
    ASSERT(dir != NULL);

    XlspConfig config;
    memset(&config, 0, sizeof(config));

    bool ok = xlsp_config_load_from_toml(&config, dir);
    ASSERT(ok);

    // Expect 3 ignore patterns
    ASSERT_EQ(config.ignore_pattern_count, 3);

    xlsp_config_free_ignores(&config);
    cleanup_temp_toml(dir);
}

TEST(toml_load_inlay_hints) {
    char *dir = create_temp_toml("[lsp]\n"
                                 "inlay_hints_type_annotations = false\n"
                                 "inlay_hints_parameter_names = false\n");
    ASSERT(dir != NULL);

    XlspConfig config;
    memset(&config, 0, sizeof(config));
    config.inlay_hints_type_annotations = true;
    config.inlay_hints_parameter_names = true;

    bool ok = xlsp_config_load_from_toml(&config, dir);
    ASSERT(ok);

    ASSERT_EQ(config.inlay_hints_type_annotations, false);
    ASSERT_EQ(config.inlay_hints_parameter_names, false);

    xlsp_config_free_ignores(&config);
    cleanup_temp_toml(dir);
}

TEST(toml_load_missing_file) {
    XlspConfig config;
    memset(&config, 0, sizeof(config));

    bool ok = xlsp_config_load_from_toml(&config, "/nonexistent/path");
    ASSERT(!ok);
}

TEST(toml_load_no_lsp_section) {
    char *dir = create_temp_toml("[build]\n"
                                 "target = \"native\"\n");
    ASSERT(dir != NULL);

    XlspConfig config;
    memset(&config, 0, sizeof(config));

    bool ok = xlsp_config_load_from_toml(&config, dir);
    ASSERT(!ok);

    cleanup_temp_toml(dir);
}

TEST(toml_load_partial_config) {
    // Only some fields present â€?others should remain at their preset values
    char *dir = create_temp_toml("[lsp]\n"
                                 "format_tab_size = 8\n");
    ASSERT(dir != NULL);

    XlspConfig config;
    memset(&config, 0, sizeof(config));
    config.diagnostics_enabled = true;
    config.diagnostic_debounce_ms = 300;
    config.format_tab_size = 4;

    bool ok = xlsp_config_load_from_toml(&config, dir);
    ASSERT(ok);

    // Only format_tab_size should change
    ASSERT_EQ(config.format_tab_size, 8);
    // Others stay at pre-set values
    ASSERT_EQ(config.diagnostics_enabled, true);
    ASSERT_EQ(config.diagnostic_debounce_ms, 300);

    xlsp_config_free_ignores(&config);
    cleanup_temp_toml(dir);
}

TEST(toml_null_safety) {
    ASSERT_EQ(xlsp_config_load_from_toml(NULL, "/tmp"), false);

    XlspConfig config;
    memset(&config, 0, sizeof(config));
    ASSERT_EQ(xlsp_config_load_from_toml(&config, NULL), false);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    (void) argc;
    (void) argv;

    printf("\n=== LSP Configuration Unit Tests ===\n\n");

    printf("Default configuration tests:\n");
    RUN_TEST(config_defaults);

    printf("\nIgnore pattern tests:\n");
    RUN_TEST(ignore_add_pattern);
    RUN_TEST(ignore_glob_pattern);
    RUN_TEST(ignore_should_ignore_hidden);
    RUN_TEST(ignore_exact_match);
    RUN_TEST(ignore_glob_match);
    RUN_TEST(ignore_multiple_patterns);
    RUN_TEST(ignore_capacity_growth);
    RUN_TEST(ignore_null_safety);

    printf("\nTOML loading tests:\n");
    RUN_TEST(toml_load_basic);
    RUN_TEST(toml_load_ignore_array);
    RUN_TEST(toml_load_inlay_hints);
    RUN_TEST(toml_load_missing_file);
    RUN_TEST(toml_load_no_lsp_section);
    RUN_TEST(toml_load_partial_config);
    RUN_TEST(toml_null_safety);

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
