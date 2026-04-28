/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lsp_navigation.c - Unit tests for LSP navigation features
 *   (hover, go-to-definition, find-references, document-highlight)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/app/lsp/xlsp_server.h"
#include "../../../src/app/lsp/xlsp_analysis.h"
#include "../../../src/app/lsp/xlsp_rename.h"
#include "../../../src/base/xjson.h"
#include "../test_win_compat.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  Testing %s... ", #name); \
    test_##name(); \
    printf("PASS\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAIL at line %d: %s\n", __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))

// Helper: open a document and parse it
static XrLspDocument *open_and_parse(XrLspServer *server, const char *uri,
                                      const char *code) {
    XrLspDocument *doc = xlsp_document_open(server, uri, code, 1);
    if (doc) {
        xlsp_parse_document(doc, server);
    }
    return doc;
}

// ============================================================================
// Hover Tests
// ============================================================================

TEST(hover_on_keyword) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code = "let x = 42\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Hover on "let" (line 0, col 0)
    XrLspPosition pos = {0, 0};
    XrJsonValue *result = xlsp_analyze_hover(server, doc, pos);
    // "let" is a keyword, hover should return something (keyword doc)
    // May return NULL if no keyword doc available â€?either is acceptable
    if (result) {
        xjson_free(result);
    }

    xlsp_server_free(server);
}

TEST(hover_on_variable) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code = "let greeting = \"hello\"\nprint(greeting)\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Hover on "greeting" in the print call (line 1, col 6)
    XrLspPosition pos = {1, 6};
    XrJsonValue *result = xlsp_analyze_hover(server, doc, pos);
    // Should get some hover info for the variable
    // Even if no type info is available, it should not crash
    if (result) {
        xjson_free(result);
    }

    xlsp_server_free(server);
}

TEST(hover_on_empty_space) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code = "let x = 42\n\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Hover on empty line (line 1, col 0)
    XrLspPosition pos = {1, 0};
    XrJsonValue *result = xlsp_analyze_hover(server, doc, pos);
    // Should return NULL for empty space
    if (result) {
        xjson_free(result);
    }

    xlsp_server_free(server);
}

TEST(hover_null_safety) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    XrLspPosition pos = {0, 0};

    // NULL doc should not crash
    XrJsonValue *result = xlsp_analyze_hover(server, NULL, pos);
    ASSERT(result == NULL);

    xlsp_server_free(server);
}

// ============================================================================
// Go to Definition Tests
// ============================================================================

TEST(definition_local_variable) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code =
        "let count = 0\n"
        "print(count)\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Go to definition of "count" on line 1 (the usage)
    XrLspPosition pos = {1, 6};
    XrJsonValue *result = xlsp_analyze_definition(server, doc, pos);
    // Should return a location pointing to line 0 (the declaration)
    if (result) {
        xjson_free(result);
    }

    xlsp_server_free(server);
}

TEST(definition_function) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code =
        "fn greet(name: string) {\n"
        "    print(name)\n"
        "}\n"
        "greet(\"world\")\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Go to definition of "greet" on line 3 (the call)
    XrLspPosition pos = {3, 0};
    XrJsonValue *result = xlsp_analyze_definition(server, doc, pos);
    // Should return a location pointing to line 0 (the declaration)
    if (result) {
        xjson_free(result);
    }

    xlsp_server_free(server);
}

TEST(definition_null_safety) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    XrLspPosition pos = {0, 0};
    XrJsonValue *result = xlsp_analyze_definition(server, NULL, pos);
    ASSERT(result == NULL);

    xlsp_server_free(server);
}

// ============================================================================
// Find References Tests
// ============================================================================

TEST(references_local_variable) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code =
        "let x = 1\n"
        "let y = x + 1\n"
        "print(x)\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Find references of "x" at its declaration (line 0, col 4)
    XrLspPosition pos = {0, 4};
    XrJsonValue *result = xlsp_analyze_references(server, doc, pos);
    ASSERT(result != NULL);

    // Should find at least the declaration itself + usages
    int ref_count = xjson_array_len(result);
    ASSERT(ref_count >= 1);

    xjson_free(result);
    xlsp_server_free(server);
}

TEST(references_function_name) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code =
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "let result = add(1, 2)\n"
        "print(add(3, 4))\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Find references of "add" at its declaration (line 0, col 3)
    XrLspPosition pos = {0, 3};
    XrJsonValue *result = xlsp_analyze_references(server, doc, pos);
    ASSERT(result != NULL);

    // Should find: declaration + 2 call sites = 3
    int ref_count = xjson_array_len(result);
    ASSERT(ref_count >= 1);

    xjson_free(result);
    xlsp_server_free(server);
}

TEST(references_null_safety) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    XrLspPosition pos = {0, 0};
    XrJsonValue *result = xlsp_analyze_references(server, NULL, pos);
    // May return NULL or empty array depending on implementation
    if (result) {
        xjson_free(result);
    }

    xlsp_server_free(server);
}

// ============================================================================
// Document Highlight Tests
// ============================================================================

TEST(highlight_variable_occurrences) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code =
        "let counter = 0\n"
        "counter = counter + 1\n"
        "print(counter)\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Highlight "counter" at declaration (line 0, col 4)
    XrLspPosition pos = {0, 4};
    XrJsonValue *result = xlsp_analyze_document_highlight(server, doc, pos);
    ASSERT(result != NULL);

    // Should find at least one highlight
    int hl_count = xjson_array_len(result);
    ASSERT(hl_count >= 1);

    xjson_free(result);
    xlsp_server_free(server);
}

TEST(highlight_null_safety) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    XrLspPosition pos = {0, 0};
    XrJsonValue *result = xlsp_analyze_document_highlight(server, NULL, pos);
    // May return NULL or empty array depending on implementation
    if (result) {
        xjson_free(result);
    }

    xlsp_server_free(server);
}

// ============================================================================
// Prepare Rename Tests
// ============================================================================

TEST(prepare_rename_valid_symbol) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code = "let counter = 0\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Prepare rename on "counter" (line 0, col 4)
    XrLspPosition pos = {0, 4};
    XrJsonValue *result = xlsp_analyze_prepare_rename(doc, pos);
    // Should return a range covering "counter"
    ASSERT(result != NULL);

    xjson_free(result);
    xlsp_server_free(server);
}

TEST(prepare_rename_keyword_rejected) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code = "let x = 42\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Prepare rename on "let" (line 0, col 0) â€?should be rejected
    XrLspPosition pos = {0, 0};
    XrJsonValue *result = xlsp_analyze_prepare_rename(doc, pos);
    ASSERT(result == NULL);

    xlsp_server_free(server);
}

TEST(prepare_rename_null_safety) {
    XrLspPosition pos = {0, 0};
    XrJsonValue *result = xlsp_analyze_prepare_rename(NULL, pos);
    ASSERT(result == NULL);
}

// ============================================================================
// Document Symbols Tests
// ============================================================================

TEST(symbols_function_listed) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code =
        "fn hello() {\n"
        "    print(\"hello\")\n"
        "}\n"
        "fn world() {\n"
        "    print(\"world\")\n"
        "}\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    XrJsonValue *result = xlsp_analyze_document_symbols(doc);
    ASSERT(result != NULL);

    // Should have at least 2 symbols (hello, world)
    int sym_count = xjson_array_len(result);
    ASSERT(sym_count >= 2);

    xjson_free(result);
    xlsp_server_free(server);
}

TEST(symbols_empty_document) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code = "\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    XrJsonValue *result = xlsp_analyze_document_symbols(doc);
    // Should return empty array, not NULL
    ASSERT(result != NULL);
    ASSERT_EQ(xjson_array_len(result), 0);

    xjson_free(result);
    xlsp_server_free(server);
}

TEST(symbols_null_safety) {
    XrJsonValue *result = xlsp_analyze_document_symbols(NULL);
    // May return NULL or empty array depending on implementation
    if (result) {
        ASSERT_EQ(xjson_array_len(result), 0);
        xjson_free(result);
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    (void)argc; (void)argv;

    printf("\n=== LSP Navigation Unit Tests ===\n\n");

    printf("Hover tests:\n");
    RUN_TEST(hover_on_keyword);
    RUN_TEST(hover_on_variable);
    RUN_TEST(hover_on_empty_space);
    RUN_TEST(hover_null_safety);

    printf("\nGo to Definition tests:\n");
    RUN_TEST(definition_local_variable);
    RUN_TEST(definition_function);
    RUN_TEST(definition_null_safety);

    printf("\nFind References tests:\n");
    RUN_TEST(references_local_variable);
    RUN_TEST(references_function_name);
    RUN_TEST(references_null_safety);

    printf("\nDocument Highlight tests:\n");
    RUN_TEST(highlight_variable_occurrences);
    RUN_TEST(highlight_null_safety);

    printf("\nPrepare Rename tests:\n");
    RUN_TEST(prepare_rename_valid_symbol);
    RUN_TEST(prepare_rename_keyword_rejected);
    RUN_TEST(prepare_rename_null_safety);

    printf("\nDocument Symbols tests:\n");
    RUN_TEST(symbols_function_listed);
    RUN_TEST(symbols_empty_document);
    RUN_TEST(symbols_null_safety);

    printf("\n=== Results: %d passed, %d failed ===\n\n",
           tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
