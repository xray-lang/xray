/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lsp_diagnostics.c - Unit tests for LSP diagnostics
 *   Tests diagnostic generation, debounce scheduling, and enable/disable.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/app/lsp/xlsp_server.h"
#include "../../../src/app/lsp/xlsp_analysis.h"
#include "../../../src/app/lsp/xlsp_json.h"

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
// Diagnostics Generation Tests
// ============================================================================

TEST(diagnostics_empty_document) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", "");
    ASSERT(doc != NULL);

    XrJsonValue *diags = xlsp_analyze_diagnostics(doc);
    ASSERT(diags != NULL);
    ASSERT_EQ(xlsp_json_array_len(diags), 0);

    xlsp_json_free(diags);
    xlsp_document_close(server, "file:///test.xr");
    xlsp_server_free(server);
}

TEST(diagnostics_valid_code) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *code = "let x = 42\nlet y = x + 1\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    XrJsonValue *diags = xlsp_analyze_diagnostics(doc);
    ASSERT(diags != NULL);
    // Valid code should produce zero diagnostics
    ASSERT_EQ(xlsp_json_array_len(diags), 0);

    xlsp_json_free(diags);
    xlsp_document_close(server, "file:///test.xr");
    xlsp_server_free(server);
}

TEST(diagnostics_syntax_error) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    // Missing closing paren
    const char *code = "let x = (42\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    XrJsonValue *diags = xlsp_analyze_diagnostics(doc);
    ASSERT(diags != NULL);
    // Should produce at least one diagnostic for the parse error
    ASSERT(xlsp_json_array_len(diags) > 0);

    // Verify first diagnostic has required LSP fields
    XrJsonValue *first = xlsp_json_array_get(diags, 0);
    ASSERT(first != NULL);
    ASSERT(xlsp_json_get_object(first, "range") != NULL);
    ASSERT(xlsp_json_get_string(first, "message") != NULL);

    xlsp_json_free(diags);
    xlsp_document_close(server, "file:///test.xr");
    xlsp_server_free(server);
}

TEST(diagnostics_null_document) {
    // Passing NULL should not crash, just return empty array
    XrJsonValue *diags = xlsp_analyze_diagnostics(NULL);
    ASSERT(diags != NULL);
    ASSERT_EQ(xlsp_json_array_len(diags), 0);
    xlsp_json_free(diags);
}

// ============================================================================
// Diagnostics Enable/Disable Tests
// ============================================================================

TEST(diagnostics_disabled_config) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    // Disable diagnostics in config
    server->config.diagnostics_enabled = false;

    const char *code = "let x = (42\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // xlsp_analyze_diagnostics still produces results (it doesn't check config),
    // but xlsp_schedule_diagnostics should be a no-op when disabled.
    // Verify schedule does not enqueue.
    ASSERT_EQ(server->pending_diag_count, 0);
    xlsp_schedule_diagnostics(server, doc);
    ASSERT_EQ(server->pending_diag_count, 0);

    xlsp_document_close(server, "file:///test.xr");
    xlsp_server_free(server);
}

TEST(diagnostics_enabled_config) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    // Enable diagnostics (default)
    server->config.diagnostics_enabled = true;

    const char *code = "let x = 42\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Schedule should enqueue the document
    xlsp_schedule_diagnostics(server, doc);
    ASSERT(server->pending_diag_count > 0);

    xlsp_document_close(server, "file:///test.xr");
    xlsp_server_free(server);
}

// ============================================================================
// Diagnostics Debounce Tests
// ============================================================================

TEST(diagnostics_debounce_dedup) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    server->config.diagnostics_enabled = true;

    const char *code = "let x = 42\n";
    XrLspDocument *doc = open_and_parse(server, "file:///test.xr", code);
    ASSERT(doc != NULL);

    // Schedule multiple times — should not duplicate in pending queue
    xlsp_schedule_diagnostics(server, doc);
    int count_after_first = server->pending_diag_count;
    xlsp_schedule_diagnostics(server, doc);
    xlsp_schedule_diagnostics(server, doc);
    // Count should not have grown (dedup via diag_pending flag)
    ASSERT_EQ(server->pending_diag_count, count_after_first);

    xlsp_document_close(server, "file:///test.xr");
    xlsp_server_free(server);
}

TEST(diagnostics_multiple_documents) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    server->config.diagnostics_enabled = true;

    XrLspDocument *doc1 = open_and_parse(server, "file:///a.xr", "let a = 1\n");
    XrLspDocument *doc2 = open_and_parse(server, "file:///b.xr", "let b = 2\n");
    ASSERT(doc1 != NULL);
    ASSERT(doc2 != NULL);

    xlsp_schedule_diagnostics(server, doc1);
    xlsp_schedule_diagnostics(server, doc2);
    // Both documents should be in the pending queue
    ASSERT(server->pending_diag_count >= 2);

    xlsp_document_close(server, "file:///a.xr");
    xlsp_document_close(server, "file:///b.xr");
    xlsp_server_free(server);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("test_lsp_diagnostics:\n");

    // Generation
    RUN_TEST(diagnostics_empty_document);
    RUN_TEST(diagnostics_valid_code);
    RUN_TEST(diagnostics_syntax_error);
    RUN_TEST(diagnostics_null_document);

    // Enable/Disable
    RUN_TEST(diagnostics_disabled_config);
    RUN_TEST(diagnostics_enabled_config);

    // Debounce
    RUN_TEST(diagnostics_debounce_dedup);
    RUN_TEST(diagnostics_multiple_documents);

    printf("\n  %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
