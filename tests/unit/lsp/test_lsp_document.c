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
#include "../../../src/app/lsp/xlsp_analysis.h"
#include "../../../src/app/lsp/xlsp_builtins.h"
#include "../../../src/app/lsp/xlsp_code_action.h"
#include "../../../src/app/lsp/xlsp_completion.h"
#include "../../../src/base/xjson.h"
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

static bool json_array_contains_label(XrJsonValue *items, const char *label) {
    if (!items || !label)
        return false;
    int n = xjson_array_len(items);
    for (int i = 0; i < n; i++) {
        XrJsonValue *item = xjson_array_get(items, i);
        const char *candidate = xjson_get_string(item, "label");
        if (candidate && strcmp(candidate, label) == 0)
            return true;
    }
    return false;
}

static XrJsonValue *json_array_find_label(XrJsonValue *items, const char *label) {
    if (!items || !label)
        return NULL;
    int n = xjson_array_len(items);
    for (int i = 0; i < n; i++) {
        XrJsonValue *item = xjson_array_get(items, i);
        const char *candidate = xjson_get_string(item, "label");
        if (candidate && strcmp(candidate, label) == 0)
            return item;
    }
    return NULL;
}

static const char *hover_markdown_value(XrJsonValue *hover) {
    XrJsonValue *contents = xjson_get_object(hover, "contents");
    return contents ? xjson_get_string(contents, "value") : NULL;
}

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

    const char *content = "var x = 1\nvar y = 2\n";
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

    // "var x = 1\nvar y = 2\n"
    // Line 0: "var x = 1\n" (10 chars)
    // Line 1: "var y = 2\n" (10 chars)
    const char *content = "var x = 1\nvar y = 2\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", content, 1);
    ASSERT(doc != NULL);

    XrLspPosition pos0 = {0, 0};  // Start of line 0
    uint32_t offset0 = xlsp_position_to_offset(doc, pos0);
    ASSERT_EQ(offset0, 0);

    XrLspPosition pos1 = {0, 4};  // "var x" -> position of 'x'
    uint32_t offset1 = xlsp_position_to_offset(doc, pos1);
    ASSERT_EQ(offset1, 4);

    XrLspPosition pos2 = {1, 0};  // Start of line 1
    uint32_t offset2 = xlsp_position_to_offset(doc, pos2);
    ASSERT_EQ(offset2, 10);

    XrLspPosition pos3 = {1, 4};  // "var y" -> position of 'y' on line 1
    uint32_t offset3 = xlsp_position_to_offset(doc, pos3);
    ASSERT_EQ(offset3, 14);

    xlsp_server_free(server);
}

TEST(offset_to_position) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "var x = 1\nvar y = 2\n";
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

    const char *content = "fn main() {\n    var x = 1\n    print(x)\n}\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///test.xr", content, 1);
    ASSERT(doc != NULL);

    // Test roundtrip for valid positions only (within line bounds)
    // Line 0: "fn main() {" (11 chars)
    // Line 1: "    var x = 1" (13 chars)
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
// Completion Tests
// ============================================================================

TEST(completion_shared_channel_member) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "shared ch = Channel<int>(1)\n"
                          "ch.\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///completion.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {1, 3};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "send"));
    ASSERT(json_array_contains_label(items, "recv"));

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(completion_u8_array_registry_methods) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "var bytes = Array<byte>(0)\n"
                          "bytes.\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///u8_array.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {1, 6};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "appendFrom"));
    ASSERT(json_array_contains_label(items, "repeatFrom"));
    ASSERT(json_array_contains_label(items, "push"));
    XrJsonValue *append = json_array_find_label(items, "appendFrom");
    ASSERT(append != NULL);
    const char *doc_text = xjson_get_string(append, "documentation");
    ASSERT(doc_text != NULL);
    ASSERT(strstr(doc_text, "Array<byte> byte bulk methods") != NULL);
    ASSERT(strstr(doc_text, "Availability: heap-capable profiles") != NULL);
    ASSERT(strstr(doc_text, "Lowering: xi.byte.array.append.from") != NULL);

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(completion_uint8_array_uses_canonical_byte_docs) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "var bytes = Array<uint8>(0)\n"
                          "bytes.\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///uint8_array.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {1, 6};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    XrJsonValue *append = json_array_find_label(items, "appendFrom");
    ASSERT(append != NULL);
    const char *doc_text = xjson_get_string(append, "documentation");
    ASSERT(doc_text != NULL);
    ASSERT(strstr(doc_text, "Array<byte> byte bulk methods") != NULL);
    ASSERT(strstr(doc_text, "Array<uint8>") == NULL);
    ASSERT(strstr(doc_text, "Slice<uint8>") == NULL);

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(completion_int_array_excludes_u8_registry_methods) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "var ints = Array<int>(0)\n"
                          "ints.\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///int_array.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {1, 5};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "push"));
    ASSERT(!json_array_contains_label(items, "appendFrom"));
    ASSERT(!json_array_contains_label(items, "repeatFrom"));

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(completion_u8_slice_registry_methods) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "var bytes = Array<byte>(4)\n"
                          "var view: Slice<byte> = bytes[:]\n"
                          "view.\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///u8_slice.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {2, 5};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "load"));
    ASSERT(json_array_contains_label(items, "store"));
    ASSERT(json_array_contains_label(items, "reinterpret"));
    ASSERT(json_array_contains_label(items, "asBytes"));

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(hover_u8_array_registry_method) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "var bytes = Array<byte>(0)\n"
                          "bytes.appendFrom(bytes[:])\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///hover_u8_array.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {1, 8};
    XrJsonValue *hover = xlsp_analyze_hover(server, doc, pos);
    ASSERT(hover != NULL);
    const char *value = hover_markdown_value(hover);
    ASSERT(value != NULL);
    ASSERT(strstr(value, "Array<byte>.appendFrom") != NULL);
    ASSERT(strstr(value, "Slice<byte>") != NULL);
    ASSERT(strstr(value, "Array<byte> byte bulk methods") != NULL);
    ASSERT(strstr(value, "Availability: heap-capable profiles") != NULL);
    ASSERT(strstr(value, "Lowering: xi.byte.array.append.from") != NULL);

    xjson_free(hover);
    xlsp_server_free(server);
}

TEST(signature_help_u8_array_registry_method) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "var bytes = Array<byte>(0)\n"
                          "bytes.appendFrom(bytes[:])\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///sig_u8_array.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {1, 17};
    XrJsonValue *help = xlsp_analyze_signature_help(doc, pos);
    ASSERT(help != NULL);
    XrJsonValue *signatures = xjson_get_array(help, "signatures");
    ASSERT(signatures != NULL);
    XrJsonValue *sig0 = xjson_array_get(signatures, 0);
    ASSERT(sig0 != NULL);
    const char *label = xjson_get_string(sig0, "label");
    ASSERT(label != NULL);
    ASSERT(strstr(label, "appendFrom") != NULL);
    ASSERT(strstr(label, "Slice<byte>") != NULL);

    xjson_free(help);
    xlsp_server_free(server);
}

TEST(builtin_generic_array_uses_error_placeholder) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    XrLspDocument *doc =
        xlsp_document_open(server, "file:///generic_array_completion.xr", "Array.\n", 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {0, 6};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    XrJsonValue *push = json_array_find_label(items, "push");
    ASSERT(push != NULL);
    const char *detail = xjson_get_string(push, "detail");
    ASSERT(detail != NULL);
    ASSERT(strstr(detail, "unknown") == NULL);
    ASSERT(strstr(detail, "<error>") != NULL);

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(global_type_query_builtins_use_canonical_names) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "typeOf(1)\n"
                          "typeName(1)\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///type_query.xr", content, 1);
    ASSERT(doc != NULL);

    XrLspPosition completion_pos = {0, 0};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, completion_pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "typeOf"));
    ASSERT(json_array_contains_label(items, "typeName"));
    ASSERT(!json_array_contains_label(items, "typeof"));
    ASSERT(!json_array_contains_label(items, "typename"));
    xjson_free(items);

    XrLspPosition type_of_hover_pos = {0, 1};
    XrJsonValue *type_of_hover = xlsp_analyze_hover(server, doc, type_of_hover_pos);
    ASSERT(type_of_hover != NULL);
    const char *type_of_hover_text = hover_markdown_value(type_of_hover);
    ASSERT(type_of_hover_text != NULL);
    ASSERT(strstr(type_of_hover_text, "typeOf(value): Type") != NULL);
    ASSERT(strstr(type_of_hover_text, "typeof") == NULL);
    xjson_free(type_of_hover);

    XrLspPosition type_name_hover_pos = {1, 1};
    XrJsonValue *type_name_hover = xlsp_analyze_hover(server, doc, type_name_hover_pos);
    ASSERT(type_name_hover != NULL);
    const char *type_name_hover_text = hover_markdown_value(type_name_hover);
    ASSERT(type_name_hover_text != NULL);
    ASSERT(strstr(type_name_hover_text, "typeName(value): string") != NULL);
    ASSERT(strstr(type_name_hover_text, "typeName<T>(): string") != NULL);
    ASSERT(strstr(type_name_hover_text, "typename") == NULL);
    xjson_free(type_name_hover);

    XrLspPosition type_of_sig_pos = {0, 7};
    XrJsonValue *type_of_sig = xlsp_analyze_signature_help(doc, type_of_sig_pos);
    ASSERT(type_of_sig != NULL);
    XrJsonValue *type_of_signatures = xjson_get_array(type_of_sig, "signatures");
    ASSERT(type_of_signatures != NULL);
    XrJsonValue *type_of_sig0 = xjson_array_get(type_of_signatures, 0);
    ASSERT(type_of_sig0 != NULL);
    const char *type_of_label = xjson_get_string(type_of_sig0, "label");
    ASSERT(type_of_label != NULL);
    ASSERT(strstr(type_of_label, "typeOf(value): Type") != NULL);
    ASSERT(strstr(type_of_label, "typeof") == NULL);
    xjson_free(type_of_sig);

    XrLspPosition type_name_sig_pos = {1, 9};
    XrJsonValue *type_name_sig = xlsp_analyze_signature_help(doc, type_name_sig_pos);
    ASSERT(type_name_sig != NULL);
    XrJsonValue *type_name_signatures = xjson_get_array(type_name_sig, "signatures");
    ASSERT(type_name_signatures != NULL);
    XrJsonValue *type_name_sig0 = xjson_array_get(type_name_signatures, 0);
    ASSERT(type_name_sig0 != NULL);
    const char *type_name_label = xjson_get_string(type_name_sig0, "label");
    ASSERT(type_name_label != NULL);
    ASSERT(strstr(type_name_label, "typeName(value): string") != NULL);
    ASSERT(strstr(type_name_label, "typeName<T>(): string") != NULL);
    ASSERT(strstr(type_name_label, "typename") == NULL);
    xjson_free(type_name_sig);

    xlsp_server_free(server);
}

TEST(param_mode_user_function_lsp_display) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "fn adjust(view: in int, slot: ref int, filled: out int) -> int {\n"
                          "    filled = view\n"
                          "    slot = slot + filled\n"
                          "    return slot\n"
                          "}\n"
                          "\n"
                          "fn main() {\n"
                          "    var value = 1\n"
                          "    var slot = 2\n"
                          "    var filled: int\n"
                          "    adjust(value, ref slot, out filled)\n"
                          "    adj\n"
                          "}\n";
    const char *expected = "fn adjust(view: in int, slot: ref int, filled: out int): int";

    XrLspDocument *doc = xlsp_document_open(server, "file:///param_mode_lsp.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition completion_pos = {11, 7};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, completion_pos);
    ASSERT(items != NULL);
    XrJsonValue *adjust_item = json_array_find_label(items, "adjust");
    ASSERT(adjust_item != NULL);
    const char *detail = xjson_get_string(adjust_item, "detail");
    ASSERT(detail != NULL);
    ASSERT_STR_EQ(detail, expected);
    xjson_free(items);

    XrLspPosition hover_pos = {10, 6};
    XrJsonValue *hover = xlsp_analyze_hover(server, doc, hover_pos);
    ASSERT(hover != NULL);
    const char *hover_text = hover_markdown_value(hover);
    ASSERT(hover_text != NULL);
    ASSERT(strstr(hover_text, expected) != NULL);
    xjson_free(hover);

    XrLspPosition sig_pos = {10, 12};
    XrJsonValue *help = xlsp_analyze_signature_help(doc, sig_pos);
    ASSERT(help != NULL);
    XrJsonValue *signatures = xjson_get_array(help, "signatures");
    ASSERT(signatures != NULL);
    XrJsonValue *sig0 = xjson_array_get(signatures, 0);
    ASSERT(sig0 != NULL);
    const char *label = xjson_get_string(sig0, "label");
    ASSERT(label != NULL);
    ASSERT_STR_EQ(label, expected);

    XrJsonValue *params = xjson_get_array(sig0, "parameters");
    ASSERT(params != NULL);
    ASSERT(xjson_array_len(params) == 3);
    ASSERT_STR_EQ(xjson_get_string(xjson_array_get(params, 0), "label"), "view: in int");
    ASSERT_STR_EQ(xjson_get_string(xjson_array_get(params, 1), "label"), "slot: ref int");
    ASSERT_STR_EQ(xjson_get_string(xjson_array_get(params, 2), "label"), "filled: out int");

    xjson_free(help);
    xlsp_server_free(server);
}

// ============================================================================
// Code Action Quick-Fix Tests (concurrency diagnostics)
// ============================================================================

// Build a minimal code_action params JSON with a single diagnostic whose
// message matches the given text.
static XrJsonValue *make_code_action_params(const char *uri, int line, int start_col, int end_col,
                                            const char *diag_message) {
    XrJsonValue *params = xjson_new_object();

    XrJsonValue *text_doc = xjson_new_object();
    xjson_object_set(text_doc, "uri", xjson_new_string(uri));
    xjson_object_set(params, "textDocument", text_doc);

    xjson_object_set(params, "range", xjson_make_range(line, start_col, line, end_col));

    XrJsonValue *context = xjson_new_object();
    XrJsonValue *diagnostics = xjson_new_array();
    XrJsonValue *diag = xjson_new_object();
    xjson_object_set(diag, "message", xjson_new_string(diag_message));
    xjson_object_set(diag, "range", xjson_make_range(line, start_col, line, end_col));
    xjson_array_push(diagnostics, diag);
    xjson_object_set(context, "diagnostics", diagnostics);
    xjson_object_set(params, "context", context);
    return params;
}

// Return true if any action in the array has a title containing all the
// given substrings.
static bool actions_contain_title_with(XrJsonValue *actions, const char *s1, const char *s2) {
    if (!actions)
        return false;
    int n = xjson_array_len(actions);
    for (int i = 0; i < n; i++) {
        XrJsonValue *a = xjson_array_get(actions, i);
        const char *title = xjson_get_string(a, "title");
        if (!title)
            continue;
        if (s1 && !strstr(title, s1))
            continue;
        if (s2 && !strstr(title, s2))
            continue;
        return true;
    }
    return false;
}

TEST(code_action_go_capture_to_shared) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    // Line 0: fn t() {
    // Line 1:     var counter = 0
    // Line 2:     go fn() { print(counter) }()
    // Line 3: }
    const char *content = "fn t() {\n"
                          "    var counter = 0\n"
                          "    go fn() { print(counter) }()\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///t.xr", content, 1);
    ASSERT(doc != NULL);

    XrJsonValue *params =
        make_code_action_params("file:///t.xr", 2, 21, 28,
                                "go closure cannot capture mutable variable 'counter'\n"
                                "hint: use one of the following:\n"
                                "  1. pass through argument: go worker(counter)\n"
                                "  2. declare as 'shared counter = ...' for concurrent reads");

    XrJsonValue *actions = xlsp_handle_code_action(server, params);
    ASSERT(actions != NULL);
    ASSERT(actions_contain_title_with(actions, "shared", "counter"));

    xjson_free(params);
    xjson_free(actions);
    xlsp_server_free(server);
}

TEST(code_action_quickfix_skips_when_decl_missing) {
    // If the declaration line cannot be located (e.g. variable declared in
    // another file), the quick-fix helper should simply not emit an action
    // rather than produce a broken text edit.
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    // Content deliberately does NOT contain `var counter`.
    const char *content = "fn t() {\n"
                          "    print(counter)\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///n.xr", content, 1);
    ASSERT(doc != NULL);

    XrJsonValue *params = make_code_action_params(
        "file:///n.xr", 1, 10, 17, "go closure cannot capture mutable variable 'counter'");

    XrJsonValue *actions = xlsp_handle_code_action(server, params);
    ASSERT(actions != NULL);
    // Must not include a quick-fix title pointing at 'counter' since the
    // declaration was not found.
    ASSERT(!actions_contain_title_with(actions, "shared", "counter"));

    xjson_free(params);
    xjson_free(actions);
    xlsp_server_free(server);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    xr_test_suppress_dialogs();
    (void) argc;
    (void) argv;

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

    printf("\nCompletion tests:\n");
    RUN_TEST(completion_shared_channel_member);
    RUN_TEST(completion_u8_array_registry_methods);
    RUN_TEST(completion_uint8_array_uses_canonical_byte_docs);
    RUN_TEST(completion_int_array_excludes_u8_registry_methods);
    RUN_TEST(completion_u8_slice_registry_methods);
    RUN_TEST(hover_u8_array_registry_method);
    RUN_TEST(signature_help_u8_array_registry_method);
    RUN_TEST(builtin_generic_array_uses_error_placeholder);
    RUN_TEST(global_type_query_builtins_use_canonical_names);
    RUN_TEST(param_mode_user_function_lsp_display);

    printf("\nCode action concurrency quick-fix tests:\n");
    RUN_TEST(code_action_go_capture_to_shared);
    RUN_TEST(code_action_quickfix_skips_when_decl_missing);

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
