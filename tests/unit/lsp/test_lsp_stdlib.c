/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lsp_stdlib.c - Unit tests for generated LSP stdlib metadata
 */

#include <stdio.h>
#include <string.h>
#include "../../../src/app/lsp/xlsp_stdlib.h"

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

#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

TEST(generated_modules_use_real_stdlib_names) {
    int count = 0;
    const XlspModuleInfo *modules = xlsp_stdlib_get_modules(&count);
    ASSERT(modules != NULL);
    ASSERT(count >= 20);
    ASSERT(xlsp_stdlib_find_module("io") != NULL);
    ASSERT(xlsp_stdlib_find_module("url") != NULL);
    ASSERT(xlsp_stdlib_find_module("fs") == NULL);
}

TEST(generated_constants_preserve_kind_and_signature) {
    const XlspModuleInfo *encoding = xlsp_stdlib_find_module("encoding");
    ASSERT(encoding != NULL);

    const XlspSymbolInfo *le = xlsp_stdlib_find_symbol(encoding, "LE");
    ASSERT(le != NULL);
    ASSERT(le->kind == XLSP_SYM_CONSTANT);
    ASSERT_STR_EQ(le->signature, "int");
    ASSERT(le->param_count == 0);

    const XlspModuleInfo *path = xlsp_stdlib_find_module("path");
    ASSERT(path != NULL);
    const XlspSymbolInfo *sep = xlsp_stdlib_find_symbol(path, "sep");
    ASSERT(sep != NULL);
    ASSERT(sep->kind == XLSP_SYM_CONSTANT);
    ASSERT_STR_EQ(sep->signature, "string");
}

TEST(generated_methods_use_def_overlay) {
    const XlspModuleInfo *url = xlsp_stdlib_find_module("url");
    ASSERT(url != NULL);

    const XlspSymbolInfo *parse = xlsp_stdlib_find_symbol(url, "parse");
    ASSERT(parse != NULL);
    ASSERT(parse->kind == XLSP_SYM_FUNCTION);
    ASSERT_STR_EQ(parse->signature, "fn(url: string): URL");
    ASSERT(strstr(parse->documentation, "URL handle") != NULL);

    const XlspSymbolInfo *join = xlsp_stdlib_find_symbol(url, "join");
    ASSERT(join != NULL);
    ASSERT_STR_EQ(join->signature, "fn(...parts: string): string");
}

TEST(generated_handle_types_surface_as_classes) {
    const XlspModuleInfo *path = xlsp_stdlib_find_module("path");
    ASSERT(path != NULL);
    const XlspSymbolInfo *path_info = xlsp_stdlib_find_symbol(path, "PathInfo");
    ASSERT(path_info != NULL);
    ASSERT(path_info->kind == XLSP_SYM_CLASS);
    ASSERT_STR_EQ(path_info->signature, "type PathInfo");

    const XlspModuleInfo *http = xlsp_stdlib_find_module("http");
    ASSERT(http != NULL);
    const XlspSymbolInfo *request = xlsp_stdlib_find_symbol(http, "HttpRequest");
    ASSERT(request != NULL);
    ASSERT(request->kind == XLSP_SYM_CLASS);
}

int main(void) {
    printf("test_lsp_stdlib:\n");
    RUN_TEST(generated_modules_use_real_stdlib_names);
    RUN_TEST(generated_constants_preserve_kind_and_signature);
    RUN_TEST(generated_methods_use_def_overlay);
    RUN_TEST(generated_handle_types_surface_as_classes);

    printf("\n%d tests passed, %d tests failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
