/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_frontend_architecture.c - Phase 3 in-tree boundary lint
 *
 * KEY CONCEPT:
 *   Walks src/frontend/{lexer,parser,format,analyzer,codegen}, parses
 *   every #include directive, and asserts each one matches the
 *   directional dependency rules established by Phase 1 / 2 / 3 of the
 *   frontend refactor. This is the same set of rules enforced by
 *   scripts/check_frontend_arch.sh -- the in-tree version means the
 *   acceptance signal lives next to the rest of the unit suite and
 *   gets exercised by ctest, regardless of whether CI runs the script.
 *
 * WHY DUPLICATE THE SHELL SCRIPT?
 *   - The shell script is a *fast* gate: zero-compile, runs in CI
 *     before the build job. It is the canary.
 *   - The C unit test is a *contract*: lives in the codebase, is part
 *     of every dev's local test cycle (`ctest`), and survives even
 *     if a future refactor moves the script around. It also has
 *     access to the exact same path that XR_TEST_SRC_DIR points at
 *     (passed in via CMake), so cross-platform path handling is
 *     uniform.
 *   - Together they eliminate "boundary regression by stealth".
 */

#include "../test_framework.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef XR_TEST_SRC_DIR
#error "XR_TEST_SRC_DIR must be defined by CMake (path to project src/)"
#endif

/* ========== include-line scanner ========== */

// Extract the path inside an #include directive. Returns true if the
// line is an #include and writes the canonical "...what..." path into
// `out`. Whitespace and the surrounding quote / angle bracket is
// stripped. False on any non-include line.
static bool extract_include(const char *line, char *out, size_t out_cap) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "include", 7) != 0) return false;
    p += 7;
    while (*p == ' ' || *p == '\t') p++;
    char close;
    if (*p == '"')      close = '"';
    else if (*p == '<') close = '>';
    else                return false;
    p++;
    size_t i = 0;
    while (*p && *p != close && *p != '\n' && i + 1 < out_cap) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return *p == close;
}

/* ========== rule predicates ========== */

// Returns true if the include path resolves into a forbidden subtree
// for the given source file. `bad_substr` is a list of substring
// patterns; a match anywhere in the resolved include text counts.
static bool include_hits_subtree(const char *inc, const char *bad_subdir) {
    // Match either "../<bad_subdir>/" (relative form, the project's
    // dominant style) or "<bad_subdir>/" at the include start (when
    // the build system already adds an -I that makes the path absolute
    // from src/).
    char rel[128];
    snprintf(rel, sizeof rel, "%s/", bad_subdir);
    if (strstr(inc, rel) != NULL) return true;
    return false;
}

// Returns true if the include is one of the public-API headers that
// frontend internals must never reach for (xray.h, xray_isolate.h).
static bool include_is_public_api(const char *inc) {
    // Match by basename: /include/xray.h or just xray.h.
    const char *base = strrchr(inc, '/');
    base = base ? base + 1 : inc;
    return strcmp(base, "xray.h") == 0 ||
           strcmp(base, "xray_isolate.h") == 0;
}

/* ========== filesystem walker ========== */

typedef bool (*include_check_fn)(const char *file_path,
                                 const char *include_path,
                                 void *cookie);

// Walk every .c / .h file rooted at `dir` recursively. For every
// #include line, call `check`. Returns the number of `check`
// invocations that returned `true` (i.e. violations).
static int walk_includes(const char *dir, include_check_fn check,
                         void *cookie) {
    DIR *d = opendir(dir);
    if (!d) {
        printf("    walk_includes: cannot open %s\n", dir);
        return -1;
    }
    int violations = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int sub = walk_includes(path, check, cookie);
            if (sub > 0) violations += sub;
            continue;
        }
        size_t nlen = strlen(ent->d_name);
        if (nlen < 2) continue;
        const char *ext = ent->d_name + nlen - 2;
        if (strcmp(ext, ".c") != 0 && strcmp(ext, ".h") != 0) continue;
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char line[1024];
        while (fgets(line, sizeof line, fp)) {
            char inc[512];
            if (!extract_include(line, inc, sizeof inc)) continue;
            if (check(path, inc, cookie)) {
                printf("    %s: includes \"%s\"\n", path, inc);
                violations++;
            }
        }
        fclose(fp);
    }
    closedir(d);
    return violations;
}

/* ========== rule checks ========== */

static bool rule_no_subtree(const char *file, const char *inc, void *c) {
    (void)file;
    return include_hits_subtree(inc, (const char *)c);
}

static bool rule_no_public_api(const char *file, const char *inc, void *c) {
    (void)file;
    (void)c;
    return include_is_public_api(inc);
}

/* ========== TEST CASES ========== */

TEST(arch_lexer_does_not_include_runtime) {
    int v = walk_includes(XR_TEST_SRC_DIR "/frontend/lexer",
                          rule_no_subtree, (void *)"runtime");
    ASSERT_EQ_INT(v, 0);
}

TEST(arch_parser_does_not_include_analyzer) {
    int v = walk_includes(XR_TEST_SRC_DIR "/frontend/parser",
                          rule_no_subtree, (void *)"analyzer");
    ASSERT_EQ_INT(v, 0);
}

TEST(arch_format_does_not_include_analyzer) {
    int v = walk_includes(XR_TEST_SRC_DIR "/frontend/format",
                          rule_no_subtree, (void *)"analyzer");
    ASSERT_EQ_INT(v, 0);
}

TEST(arch_format_does_not_include_public_api) {
    int v = walk_includes(XR_TEST_SRC_DIR "/frontend/format",
                          rule_no_public_api, NULL);
    ASSERT_EQ_INT(v, 0);
}

TEST(arch_frontend_does_not_include_public_api) {
    int v = walk_includes(XR_TEST_SRC_DIR "/frontend",
                          rule_no_public_api, NULL);
    ASSERT_EQ_INT(v, 0);
}

TEST(arch_analyzer_does_not_include_codegen) {
    int v = walk_includes(XR_TEST_SRC_DIR "/frontend/analyzer",
                          rule_no_subtree, (void *)"codegen");
    ASSERT_EQ_INT(v, 0);
}

TEST(arch_codegen_does_not_include_format) {
    int v = walk_includes(XR_TEST_SRC_DIR "/frontend/codegen",
                          rule_no_subtree, (void *)"format");
    ASSERT_EQ_INT(v, 0);
}

/* ========== Architecture: AstNode has no compile_type field ========== */

TEST(arch_astnode_has_no_compile_type_field) {
    const char *path = XR_TEST_SRC_DIR "/frontend/parser/xast_nodes.h";
    FILE *fp = fopen(path, "r");
    ASSERT_NOT_NULL(fp);

    int violations = 0;
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        // Skip pure-comment lines (//, /*, *) so a dust comment
        // does not trip the assertion.
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '/' || *p == '*') continue;
        // Look for an actual field declaration: `compile_type;`,
        // `compile_type[`, or `compile_type,`.
        const char *needle = strstr(p, "compile_type");
        if (!needle) continue;
        const char *suffix = needle + strlen("compile_type");
        // Allow `compile_type_DUMMY`-prefixed identifiers if they ever
        // exist by skipping over an `_legacy` adornment that itself is
        // followed by a declaration terminator.
        if (strncmp(suffix, "_legacy", 7) == 0) {
            suffix += 7;
        }
        while (*suffix == ' ' || *suffix == '\t') suffix++;
        if (*suffix == ';' || *suffix == '[' || *suffix == ',') {
            printf("    declaration found in %s: %s", path, line);
            violations++;
        }
    }
    fclose(fp);
    ASSERT_EQ_INT(violations, 0);
}

/* ========== Driver ========== */

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("frontend architecture (Phase 3)");
    RUN_TEST(arch_lexer_does_not_include_runtime);
    RUN_TEST(arch_parser_does_not_include_analyzer);
    RUN_TEST(arch_format_does_not_include_analyzer);
    RUN_TEST(arch_format_does_not_include_public_api);
    RUN_TEST(arch_frontend_does_not_include_public_api);
    RUN_TEST(arch_analyzer_does_not_include_codegen);
    RUN_TEST(arch_codegen_does_not_include_format);
    RUN_TEST(arch_astnode_has_no_compile_type_field);
TEST_MAIN_END()
