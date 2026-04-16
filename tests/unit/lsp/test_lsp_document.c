/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lsp_document.c - Unit tests for LSP document management
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../src/app/lsp/xlsp_server.h"

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
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp((a), (b)) == 0)

// ============================================================================
// Server and Document Lifecycle Tests
// ============================================================================

TEST(server_create_destroy) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    ASSERT(server->initialized == false);
    ASSERT(server->doc_table != NULL);
    ASSERT(server->doc_table->doc_count == 0);
    xlsp_server_free(server);
}

TEST(document_open) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    const char *content = "let x = 1\nlet y = 2\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", content, 1);
    
    ASSERT(doc != NULL);
    ASSERT_STR_EQ(doc->uri, "file:///test.xr");
    ASSERT_EQ(doc->version, 1);
    ASSERT(doc->content != NULL);
    ASSERT_EQ(doc->length, strlen(content));
    
    xlsp_server_free(server);
}

TEST(document_get) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    xlsp_document_open(server, "file:///test1.xr", "content1", 1);
    xlsp_document_open(server, "file:///test2.xr", "content2", 1);
    
    XrLspDocument *doc1 = xlsp_document_get(server, "file:///test1.xr");
    ASSERT(doc1 != NULL);
    ASSERT_STR_EQ(doc1->uri, "file:///test1.xr");
    
    XrLspDocument *doc2 = xlsp_document_get(server, "file:///test2.xr");
    ASSERT(doc2 != NULL);
    ASSERT_STR_EQ(doc2->uri, "file:///test2.xr");
    
    XrLspDocument *doc3 = xlsp_document_get(server, "file:///nonexistent.xr");
    ASSERT(doc3 == NULL);
    
    xlsp_server_free(server);
}

TEST(document_close) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    xlsp_document_open(server, "file:///test.xr", "content", 1);
    ASSERT(xlsp_document_get(server, "file:///test.xr") != NULL);
    
    xlsp_document_close(server, "file:///test.xr");
    ASSERT(xlsp_document_get(server, "file:///test.xr") == NULL);
    
    xlsp_server_free(server);
}

TEST(document_multiple) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    xlsp_document_open(server, "file:///a.xr", "a", 1);
    xlsp_document_open(server, "file:///b.xr", "b", 1);
    xlsp_document_open(server, "file:///c.xr", "c", 1);
    
    ASSERT(xlsp_document_get(server, "file:///a.xr") != NULL);
    ASSERT(xlsp_document_get(server, "file:///b.xr") != NULL);
    ASSERT(xlsp_document_get(server, "file:///c.xr") != NULL);
    
    xlsp_document_close(server, "file:///b.xr");
    
    ASSERT(xlsp_document_get(server, "file:///a.xr") != NULL);
    ASSERT(xlsp_document_get(server, "file:///b.xr") == NULL);
    ASSERT(xlsp_document_get(server, "file:///c.xr") != NULL);
    
    xlsp_server_free(server);
}

// ============================================================================
// Line Index Tests
// ============================================================================

TEST(document_line_count) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    const char *content = "line1\nline2\nline3\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", content, 1);
    
    ASSERT(doc != NULL);
    ASSERT(doc->line_count >= 3);
    
    xlsp_server_free(server);
}

TEST(position_to_offset) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    // "let x = 1\nlet y = 2\n"
    // Line 0: "let x = 1\n" (10 chars)
    // Line 1: "let y = 2\n" (10 chars)
    const char *content = "let x = 1\nlet y = 2\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", content, 1);
    ASSERT(doc != NULL);
    
    XrLspPosition pos0 = {0, 0};  // Start of line 0
    uint32_t offset0 = xlsp_position_to_offset(doc, pos0);
    ASSERT_EQ(offset0, 0);
    
    XrLspPosition pos1 = {0, 4};  // "let x" -> position of 'x'
    uint32_t offset1 = xlsp_position_to_offset(doc, pos1);
    ASSERT_EQ(offset1, 4);
    
    XrLspPosition pos2 = {1, 0};  // Start of line 1
    uint32_t offset2 = xlsp_position_to_offset(doc, pos2);
    ASSERT_EQ(offset2, 10);
    
    XrLspPosition pos3 = {1, 4};  // "let y" -> position of 'y' on line 1
    uint32_t offset3 = xlsp_position_to_offset(doc, pos3);
    ASSERT_EQ(offset3, 14);
    
    xlsp_server_free(server);
}

TEST(offset_to_position) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    const char *content = "let x = 1\nlet y = 2\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", content, 1);
    ASSERT(doc != NULL);
    
    XrLspPosition pos0 = xlsp_offset_to_position(doc, 0);
    ASSERT_EQ(pos0.line, 0);
    ASSERT_EQ(pos0.character, 0);
    
    XrLspPosition pos4 = xlsp_offset_to_position(doc, 4);
    ASSERT_EQ(pos4.line, 0);
    ASSERT_EQ(pos4.character, 4);
    
    XrLspPosition pos10 = xlsp_offset_to_position(doc, 10);
    ASSERT_EQ(pos10.line, 1);
    ASSERT_EQ(pos10.character, 0);
    
    XrLspPosition pos14 = xlsp_offset_to_position(doc, 14);
    ASSERT_EQ(pos14.line, 1);
    ASSERT_EQ(pos14.character, 4);
    
    xlsp_server_free(server);
}

TEST(position_roundtrip) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    const char *content = "fn main() {\n    let x = 1\n    print(x)\n}\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", content, 1);
    ASSERT(doc != NULL);
    
    // Test roundtrip for valid positions only (within line bounds)
    // Line 0: "fn main() {" (11 chars)
    // Line 1: "    let x = 1" (13 chars)
    // Line 2: "    print(x)" (12 chars)
    // Line 3: "}" (1 char)
    int line_lengths[] = {11, 13, 12, 1};
    
    for (int line = 0; line < 4; line++) {
        for (int col = 0; col < line_lengths[line]; col++) {
            XrLspPosition pos = {line, col};
            uint32_t offset = xlsp_position_to_offset(doc, pos);
            XrLspPosition pos2 = xlsp_offset_to_position(doc, offset);
            ASSERT_EQ(pos.line, pos2.line);
            ASSERT_EQ(pos.character, pos2.character);
        }
    }
    
    xlsp_server_free(server);
}

// ============================================================================
// Document Version Tests
// ============================================================================

TEST(document_version) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", "content", 1);
    ASSERT_EQ(doc->version, 1);
    
    // Close and reopen with new version
    xlsp_document_close(server, "file:///test.xr");
    doc = xlsp_document_open(server, "file:///test.xr", "new content", 5);
    ASSERT_EQ(doc->version, 5);
    
    xlsp_server_free(server);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    printf("\n=== LSP Document Management Unit Tests ===\n\n");
    
    printf("Server and document lifecycle tests:\n");
    RUN_TEST(server_create_destroy);
    RUN_TEST(document_open);
    RUN_TEST(document_get);
    RUN_TEST(document_close);
    RUN_TEST(document_multiple);
    
    printf("\nLine index tests:\n");
    RUN_TEST(document_line_count);
    RUN_TEST(position_to_offset);
    RUN_TEST(offset_to_position);
    RUN_TEST(position_roundtrip);
    
    printf("\nDocument version tests:\n");
    RUN_TEST(document_version);
    
    printf("\n=== Results: %d passed, %d failed ===\n\n", 
           tests_passed, tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
