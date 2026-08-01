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
#include "../../../src/app/lsp/xlsp_cycle_report.h"
#include "../../../src/base/xmalloc.h"
#include <errno.h>
#include <sys/stat.h>
#include "../../../src/app/lsp/xlsp_completion.h"
#include "../../../src/app/lsp/xlsp_inlay_hints.h"
#include "../../../src/app/lsp/xlsp_imports.h"
#include "../../../src/app/lsp/xlsp_semantic_tokens.h"
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

static XrJsonValue *json_array_find_label_tooltip(XrJsonValue *items, const char *label,
                                                  const char *tooltip) {
    if (!items || !label || !tooltip)
        return NULL;
    int n = xjson_array_len(items);
    for (int i = 0; i < n; i++) {
        XrJsonValue *item = xjson_array_get(items, i);
        const char *candidate = xjson_get_string(item, "label");
        const char *candidate_tooltip = xjson_get_string(item, "tooltip");
        if (candidate && candidate_tooltip && strcmp(candidate, label) == 0 &&
            strcmp(candidate_tooltip, tooltip) == 0) {
            return item;
        }
    }
    return NULL;
}

static const char *hover_markdown_value(XrJsonValue *hover) {
    XrJsonValue *contents = xjson_get_object(hover, "contents");
    return contents ? xjson_get_string(contents, "value") : NULL;
}

static bool content_position_of_nth(const char *content, const char *needle, int occurrence,
                                    int *out_line, int *out_col) {
    if (!content || !needle || occurrence <= 0)
        return false;

    const char *p = content;
    for (int seen = 0; seen < occurrence; seen++) {
        p = strstr(p, needle);
        if (!p)
            return false;
        if (seen + 1 < occurrence)
            p += strlen(needle);
    }

    int line = 0;
    int col = 0;
    for (const char *scan = content; scan < p; scan++) {
        if (*scan == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    if (out_line)
        *out_line = line;
    if (out_col)
        *out_col = col;
    return true;
}

TEST(block_import_path_uses_shared_quoted_literal_decoder) {
    const char *content = "import \"\"\"\n"
                          "    ./nested/module\n"
                          "    \"\"\"\n";
    XlspImportInfo *imports = xlsp_parse_imports(content, "file:///tmp/xray214/main.xr");
    ASSERT(imports != NULL);
    ASSERT_STR_EQ(imports->import_path, "./nested/module");
    ASSERT_STR_EQ(imports->module_name, "module");
    ASSERT(imports->next == NULL);
    xlsp_free_imports(imports);
}

static bool semantic_token_exists(XlspSemanticTokensResult *tokens, int line, int col,
                                  const char *text, XlspSemanticTokenType type) {
    if (!tokens || !text)
        return false;
    int len = (int) strlen(text);
    for (int i = 0; i < tokens->count; i++) {
        XlspSemanticToken *token = &tokens->tokens[i];
        if (token->line == line && token->start_char == col && token->length == len &&
            token->type == type) {
            return true;
        }
    }
    return false;
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

    const char *content = "const ch = Channel<int>(1)\n"
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

TEST(completion_inferred_int_members) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "fn main() {\n"
                          "    var a = 10\n"
                          "    a.\n"
                          "}\n";
    XrLspDocument *doc =
        xlsp_document_open(server, "file:///completion_inferred_int.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrJsonValue *items = xlsp_analyze_completion(server, doc, (XrLspPosition) {2, 6});
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "abs"));
    ASSERT(json_array_contains_label(items, "toString"));
    ASSERT(json_array_contains_label(items, "checkedAdd"));

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(contextual_u32_literal_preserves_completion_and_hover_type) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "fn main() {\n"
                          "    var a: u32 = 10\n"
                          "    a.\n"
                          "}\n";
    XrLspDocument *doc =
        xlsp_document_open(server, "file:///completion_explicit_u32.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrJsonValue *items = xlsp_analyze_completion(server, doc, (XrLspPosition) {2, 6});
    ASSERT(items != NULL);
    XrJsonValue *rotate = json_array_find_label(items, "rotateLeft");
    ASSERT(rotate != NULL);
    ASSERT_STR_EQ(xjson_get_string(rotate, "detail"), "rotateLeft(count: int): u32");

    xjson_free(items);

    const char *hover_content = "fn value() -> u32 {\n"
                                "    var a: u32 = 10\n"
                                "    return a.rotateLeft(1)\n"
                                "}\n";
    XrLspDocument *hover_doc =
        xlsp_document_open(server, "file:///hover_contextual_u32.xr", hover_content, 1);
    ASSERT(hover_doc != NULL);
    xlsp_parse_document(hover_doc, server);
    XrJsonValue *hover = xlsp_analyze_hover(server, hover_doc, (XrLspPosition) {2, 15});
    ASSERT(hover != NULL);
    const char *hover_text = hover_markdown_value(hover);
    ASSERT(hover_text != NULL);
    ASSERT(strstr(hover_text, "u32.rotateLeft") != NULL);
    xjson_free(hover);

    xlsp_server_free(server);
}

TEST(completion_enum_static_variants_descriptor) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "enum Color { Red, Green }\n"
                          "Color.\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///enum_static.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {1, 6};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "Red"));
    ASSERT(json_array_contains_label(items, "Green"));
    XrJsonValue *variants = json_array_find_label(items, "variants");
    ASSERT(variants != NULL);
    const char *detail = xjson_get_string(variants, "detail");
    ASSERT(detail != NULL);
    ASSERT(strstr(detail, "EnumVariants<Color>") != NULL);

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(completion_enum_descriptor_properties) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "enum Event { Data(value: int) }\n"
                          "for (variant in Event.variants) {\n"
                          "    for (field in variant.payloads) {\n"
                          "        field.\n"
                          "    }\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///enum_payload_loop.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {3, 14};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "index"));
    ASSERT(json_array_contains_label(items, "name"));
    ASSERT(json_array_contains_label(items, "type"));
    ASSERT(!json_array_contains_label(items, "Data"));

    xjson_free(items);
    xlsp_server_free(server);
}

TEST(hover_enum_descriptor_keeps_precise_type) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "enum Color { Red, Green }\n"
                          "for (variant in Color.variants) {\n"
                          "    print(variant.ordinal)\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///enum_hover.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {2, 20};
    XrJsonValue *hover = xlsp_analyze_hover(server, doc, pos);
    ASSERT(hover != NULL);
    const char *value = hover_markdown_value(hover);
    ASSERT(value != NULL);
    ASSERT(strstr(value, "EnumVariant<Color>.ordinal: int") != NULL);
    ASSERT(strstr(value, "Allocation: none") != NULL);

    xjson_free(hover);
    xlsp_server_free(server);
}

TEST(completion_enum_iteration_variable_is_descriptor) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "enum Color { Red, Green }\n"
                          "for (variant in Color.variants) {\n"
                          "    variant.\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///enum_loop.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition pos = {2, 12};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, pos);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "ordinal"));
    ASSERT(json_array_contains_label(items, "payloads"));
    ASSERT(!json_array_contains_label(items, "Red"));

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

    const char *content = "var bytes = Array<u8>(0)\n"
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
    ASSERT(strstr(doc_text, "Array<u8>") == NULL);
    ASSERT(strstr(doc_text, "Slice<u8>") == NULL);

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

TEST(hover_deprecated_message_roundtrip) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "@deprecated(\"use modern\")\n"
                          "export fn legacy(x: int) -> int { return x + 1 }\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///hover_deprecated.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    int line = 0;
    int col = 0;
    ASSERT(content_position_of_nth(content, "legacy", 1, &line, &col));
    XrJsonValue *hover =
        xlsp_analyze_hover(server, doc, (XrLspPosition) {(uint32_t) line, (uint32_t) col});
    ASSERT(hover != NULL);
    const char *text = hover_markdown_value(hover);
    ASSERT(text != NULL);
    ASSERT(strstr(text, "fn legacy") != NULL);
    ASSERT(strstr(text, "Deprecated: use modern.") != NULL);
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

TEST(completion_public_attributes_use_registry) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    XrLspDocument *doc = xlsp_document_open(server, "file:///attributes.xr", "\n", 1);
    ASSERT(doc != NULL);

    XrJsonValue *items = xlsp_analyze_completion(server, doc, (XrLspPosition) {0, 0});
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "@test"));
    ASSERT(json_array_contains_label(items, "@before_each"));
    ASSERT(json_array_contains_label(items, "@after_each"));
    ASSERT(json_array_contains_label(items, "@before_all"));
    ASSERT(json_array_contains_label(items, "@after_all"));
    ASSERT(json_array_contains_label(items, "@deprecated"));
    ASSERT(json_array_contains_label(items, "@derive"));
    ASSERT(!json_array_contains_label(items, "@no_alloc"));
    ASSERT(!json_array_contains_label(items, "@native"));
    ASSERT(!json_array_contains_label(items, "@c_export"));
    xjson_free(items);
    xlsp_server_free(server);
}

TEST(exact_integer_bit_builtins_use_receiver_specialized_registry) {
    XrType u32;
    memset(&u32, 0, sizeof(u32));
    u32.kind = XR_KIND_INT;
    u32.scalar_rep = XR_NATIVE_U32;

    XrJsonValue *items = xlsp_builtin_get_completions_for_type(&u32);
    ASSERT(items != NULL);
    ASSERT(json_array_contains_label(items, "rotateLeft"));
    ASSERT(json_array_contains_label(items, "rotateRight"));
    ASSERT(json_array_contains_label(items, "byteswap"));
    ASSERT(json_array_contains_label(items, "popcount"));
    ASSERT(json_array_contains_label(items, "leadingZeros"));
    ASSERT(json_array_contains_label(items, "trailingZeros"));

    XrJsonValue *rotate = json_array_find_label(items, "rotateLeft");
    ASSERT(rotate != NULL);
    ASSERT_STR_EQ(xjson_get_string(rotate, "detail"), "rotateLeft(count: int): u32");
    const char *documentation = xjson_get_string(rotate, "documentation");
    ASSERT(documentation != NULL);
    ASSERT(strstr(documentation, "Allocation: no heap allocation") != NULL);
    ASSERT(strstr(documentation, "Lowering: xi.bit.rotl") != NULL);

    char signature[160];
    ASSERT(xlsp_builtin_get_signature_for_type(&u32, "byteswap", signature, sizeof(signature)) !=
           NULL);
    ASSERT_STR_EQ(signature, "byteswap(): u32");
    ASSERT(xlsp_builtin_get_signature_for_type(&u32, "popcount", signature, sizeof(signature)) !=
           NULL);
    ASSERT_STR_EQ(signature, "popcount(): int");

    xjson_free(items);
}

TEST(param_mode_user_function_lsp_display) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "fn adjust(view: int, slot: ref int, payload: move Array<int>) -> int {\n"
                          "    slot = slot + view + len(payload)\n"
                          "    return slot\n"
                          "}\n"
                          "\n"
                          "fn main() {\n"
                          "    var value = 1\n"
                          "    var slot = 2\n"
                          "    var payload = [3]\n"
                          "    adjust(value, ref slot, move payload)\n"
                          "    adj\n"
                          "}\n";
    const char *expected = "fn adjust(view: int, slot: ref int, payload: move Array<int>): int";

    XrLspDocument *doc = xlsp_document_open(server, "file:///param_mode_lsp.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspPosition completion_pos = {10, 7};
    XrJsonValue *items = xlsp_analyze_completion(server, doc, completion_pos);
    ASSERT(items != NULL);
    XrJsonValue *adjust_item = json_array_find_label(items, "adjust");
    ASSERT(adjust_item != NULL);
    const char *detail = xjson_get_string(adjust_item, "detail");
    ASSERT(detail != NULL);
    ASSERT_STR_EQ(detail, expected);
    xjson_free(items);

    XrLspPosition hover_pos = {9, 6};
    XrJsonValue *hover = xlsp_analyze_hover(server, doc, hover_pos);
    ASSERT(hover != NULL);
    const char *hover_text = hover_markdown_value(hover);
    ASSERT(hover_text != NULL);
    ASSERT(strstr(hover_text, expected) != NULL);
    xjson_free(hover);

    XrLspPosition sig_pos = {9, 12};
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
    ASSERT_STR_EQ(xjson_get_string(xjson_array_get(params, 0), "label"), "view: int");
    ASSERT_STR_EQ(xjson_get_string(xjson_array_get(params, 1), "label"), "slot: ref int");
    ASSERT_STR_EQ(xjson_get_string(xjson_array_get(params, 2), "label"),
                  "payload: move Array<int>");

    xjson_free(help);
    xlsp_server_free(server);
}

TEST(param_mode_semantic_tokens_mark_modes_and_call_access) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "fn adjust(view: int, slot: ref int, payload: move Array<int>) -> int {\n"
                          "    slot = slot + view + len(payload)\n"
                          "    return slot\n"
                          "}\n"
                          "\n"
                          "fn main() {\n"
                          "    var value = 1\n"
                          "    var slot = 2\n"
                          "    var payload = [3]\n"
                          "    adjust(value, ref slot, move payload)\n"
                          "}\n";

    XrLspDocument *doc = xlsp_document_open(server, "file:///param_mode_tokens.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XlspSemanticTokensResult *tokens = xlsp_analyze_semantic_tokens(doc);
    ASSERT(tokens != NULL);

    int line = -1;
    int col = -1;
    ASSERT(content_position_of_nth(content, "ref", 1, &line, &col));
    ASSERT(semantic_token_exists(tokens, line, col, "ref", XLSP_TOKEN_MODIFIER));
    ASSERT(content_position_of_nth(content, "move", 1, &line, &col));
    ASSERT(semantic_token_exists(tokens, line, col, "move", XLSP_TOKEN_MODIFIER));
    ASSERT(content_position_of_nth(content, "ref", 2, &line, &col));
    ASSERT(semantic_token_exists(tokens, line, col, "ref", XLSP_TOKEN_MODIFIER));
    ASSERT(content_position_of_nth(content, "move", 2, &line, &col));
    ASSERT(semantic_token_exists(tokens, line, col, "move", XLSP_TOKEN_MODIFIER));

    xlsp_semantic_tokens_free(tokens);
    xlsp_server_free(server);
}

TEST(block_quoted_literals_preserve_lsp_positions_and_semantic_boundaries) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    const char *content = "var page = r\"\"\"\n"
                          "<div title=\"quoted\">{ not_xray }</div>\n"
                          "\"\"\"\n"
                          "var bytes = br\"\"\"\"\n"
                          "${HOME} // literal { bytes }\n"
                          "\"\"\"\"\n"
                          "var after = 7\n";
    XrLspDocument *doc =
        xlsp_document_open(server, "file:///quoted_literal_positions.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);
    ASSERT(doc->ast != NULL);

    int after_line = -1;
    int after_col = -1;
    ASSERT(content_position_of_nth(content, "after", 1, &after_line, &after_col));
    XrLspPosition after_pos = {after_line, after_col};
    uint32_t after_offset = xlsp_position_to_offset(doc, after_pos);
    XrLspPosition roundtrip = xlsp_offset_to_position(doc, after_offset);
    ASSERT_EQ(roundtrip.line, (uint32_t) after_line);
    ASSERT_EQ(roundtrip.character, (uint32_t) after_col);

    XlspSemanticTokensResult *tokens = xlsp_analyze_semantic_tokens(doc);
    ASSERT(tokens != NULL);
    ASSERT(semantic_token_exists(tokens, after_line, after_col, "after", XLSP_TOKEN_VARIABLE));
    for (int i = 0; i < tokens->count; i++) {
        ASSERT(tokens->tokens[i].line != 1);
        ASSERT(tokens->tokens[i].line != 4);
    }

    xlsp_semantic_tokens_free(tokens);
    xlsp_server_free(server);
}

TEST(param_mode_inlay_hints_describe_modes) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *content = "fn adjust(view: int, slot: ref int, payload: move Array<int>) -> int {\n"
                          "    slot = slot + view + len(payload)\n"
                          "    return slot\n"
                          "}\n"
                          "\n"
                          "fn main() {\n"
                          "    var value = 1\n"
                          "    var target = 2\n"
                          "    var payload = [3]\n"
                          "    adjust(value, ref target, move payload)\n"
                          "}\n";

    XrLspDocument *doc = xlsp_document_open(server, "file:///param_mode_inlay.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspRange range = {{0, 0}, {20, 0}};
    XrJsonValue *hints = xlsp_analyze_inlay_hints(server, doc, range);
    ASSERT(hints != NULL);
    ASSERT(json_array_find_label(hints, "view:") != NULL);
    ASSERT(json_array_find_label_tooltip(hints, "slot:", "parameter mode: ref") != NULL);
    ASSERT(json_array_find_label_tooltip(hints, "payload:", "parameter mode: move") != NULL);

    xjson_free(hints);
    xlsp_server_free(server);
}

TEST(throw_effect_inlay_hints_show_inferred_result) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    const char *content = "enum HintError { Boom }\n"
                          "fn pure(value: int) -> int { return value + 1 }\n"
                          "fn fallible() { throw HintError.Boom }\n";
    XrLspDocument *doc = xlsp_document_open(server, "file:///throw_effect_inlay.xr", content, 1);
    ASSERT(doc != NULL);
    xlsp_parse_document(doc, server);

    XrLspRange range = {{0, 0}, {10, 0}};
    XrJsonValue *hints = xlsp_analyze_inlay_hints(server, doc, range);
    ASSERT(hints != NULL);
    ASSERT(json_array_find_label_tooltip(hints, " · no_throw ✓", "inferred error effect") != NULL);
    ASSERT(json_array_find_label_tooltip(hints, " · may throw {HintError}",
                                         "inferred error effect") != NULL);
    xjson_free(hints);
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

static XrJsonValue *find_action_with_title(XrJsonValue *actions, const char *needle) {
    if (!actions || !needle)
        return NULL;
    int n = xjson_array_len(actions);
    for (int i = 0; i < n; i++) {
        XrJsonValue *action = xjson_array_get(actions, i);
        const char *title = xjson_get_string(action, "title");
        if (title && strstr(title, needle))
            return action;
    }
    return NULL;
}

TEST(code_action_payload_enum_iteration_to_variants) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *uri = "file:///enum_quickfix.xr";
    const char *content = "enum Result { Ok(value: int), Error(message: string) }\n"
                          "for (value in Result) {\n"
                          "    print(value)\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, uri, content, 1);
    ASSERT(doc != NULL);

    XrJsonValue *params = make_code_action_params(
        uri, 1, 0, 21,
        "payload enum 'Result' is not directly iterable because its value set is not finite; "
        "use `Result.variants` to iterate variant descriptors, or construct concrete payload "
        "values explicitly");
    XrJsonValue *actions = xlsp_handle_code_action(server, params);
    ASSERT(actions != NULL);
    XrJsonValue *action = find_action_with_title(actions, "Result.variants");
    ASSERT(action != NULL);
    XrJsonValue *edit = xjson_get_object(action, "edit");
    XrJsonValue *changes = edit ? xjson_get_object(edit, "changes") : NULL;
    XrJsonValue *edits = changes ? xjson_get_array(changes, uri) : NULL;
    ASSERT(edits != NULL);
    ASSERT_EQ(xjson_array_len(edits), 1);
    XrJsonValue *text_edit = xjson_array_get(edits, 0);
    ASSERT_STR_EQ(xjson_get_string(text_edit, "newText"), ".variants");
    XrJsonValue *range = xjson_get_object(text_edit, "range");
    XrJsonValue *start = range ? xjson_get_object(range, "start") : NULL;
    ASSERT(start != NULL);
    ASSERT_EQ(xjson_get_int(start, "line"), 1);
    ASSERT_EQ(xjson_get_int(start, "character"), 20);

    xjson_free(actions);
    xjson_free(params);
    xlsp_server_free(server);
}

TEST(code_action_go_capture_has_no_keyword_rewrite) {
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

    XrJsonValue *params = make_code_action_params(
        "file:///t.xr", 2, 21, 28,
        "go closure cannot capture mutable variable 'counter'\n"
        "hint: bind a Channel/Atomic/Mutex handle as const, or transfer one owner with "
        "go worker(move counter)");

    XrJsonValue *actions = xlsp_handle_code_action(server, params);
    ASSERT(actions != NULL);
    // Concurrent ownership cannot be repaired by replacing `var` with a
    // keyword. The user must choose a state-owner channel, a synchronized
    // handle, or an explicit single-owner transfer.
    ASSERT(!actions_contain_title_with(actions, "shared", "counter"));

    xjson_free(params);
    xjson_free(actions);
    xlsp_server_free(server);
}

// ============================================================================
// Main
// ============================================================================

// ============================================================================
// Reference-cycle code actions (task 247 phase H)
// ============================================================================

// Point the server's workspace at a temp dir and drop a sidecar report in it,
// the same way a development-build run of the program would.
static char g_cycle_workspace[512];

static bool write_sidecar(XrLspServer *server, const char *json) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_cycle_workspace, sizeof(g_cycle_workspace), "%s/xray-lsp-cycle-test",
             tmp ? tmp : "/tmp");
    if (mkdir(g_cycle_workspace, 0700) != 0 && errno != EEXIST)
        return false;

    server->workspace_folder_count = 1;
    xr_free(server->workspace_folders[0].path);
    server->workspace_folders[0].path = xr_strdup(g_cycle_workspace);

    char path[640];
    snprintf(path, sizeof(path), "%s/.xray-cycles.json", g_cycle_workspace);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    fputs(json, f);
    fclose(f);
    return true;
}

static void remove_sidecar(void) {
    char path[640];
    snprintf(path, sizeof(path), "%s/.xray-cycles.json", g_cycle_workspace);
    remove(path);
}

static const char *CYCLE_SIDECAR =
    "{\"version\":1,\"source\":\"cycle-detector\",\"cycles\":[{\"objects\":2,\"bytes\":112,"
    "\"edges\":[{\"from\":\"Node\",\"to\":\"Node\",\"field\":\"peer\",\"kind\":\"field\","
    "\"weak_annotatable\":true},"
    "{\"from\":\"Node\",\"to\":\"Node\",\"field\":\"owner\",\"kind\":\"field\","
    "\"weak_annotatable\":true}]}]}\n";

TEST(cycle_report_diagnoses_every_candidate_field) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    ASSERT(write_sidecar(server, CYCLE_SIDECAR));

    const char *uri = "file:///cycle.xr";
    const char *content = "class Node {\n"
                          "    peer: Node?\n"
                          "    owner: Node?\n"
                          "    tag: int\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, uri, content, 1);
    ASSERT(doc != NULL);

    // The publish path parses before it diagnoses; do the same here so the
    // walk has an AST to find the field declarations in.
    xlsp_parse_document(doc, server);

    XrJsonValue *diagnostics = xjson_new_array();
    xlsp_cycle_report_refresh(server);
    xlsp_cycle_report_diagnostics(doc, diagnostics);

    // Both reported edges get a diagnostic; the field that is not on a cycle
    // gets none. Completeness is the point (247 H.5) -- a missing candidate is
    // an edge the user cannot choose.
    ASSERT_EQ(xjson_array_len(diagnostics), 2);
    bool saw_peer = false, saw_owner = false, saw_tag = false;
    for (int i = 0; i < xjson_array_len(diagnostics); i++) {
        const char *msg = xjson_get_string(xjson_array_get(diagnostics, i), "message");
        ASSERT(msg != NULL);
        if (strstr(msg, "`Node.peer`"))
            saw_peer = true;
        if (strstr(msg, "`Node.owner`"))
            saw_owner = true;
        if (strstr(msg, "`Node.tag`"))
            saw_tag = true;
    }
    ASSERT(saw_peer);
    ASSERT(saw_owner);
    ASSERT(!saw_tag);

    // Diagnostics land on the field's own declaration line, which is where the
    // `weak` insertion has to go.
    XrJsonValue *first = xjson_array_get(diagnostics, 0);
    XrJsonValue *start = xjson_get_object(xjson_get_object(first, "range"), "start");
    ASSERT_EQ(xjson_get_int(start, "line"), 1);

    xjson_free(diagnostics);
    remove_sidecar();
    xlsp_server_free(server);
}

TEST(cycle_report_skips_already_weak_field) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);
    ASSERT(write_sidecar(server, CYCLE_SIDECAR));

    const char *uri = "file:///cycle_fixed.xr";
    const char *content = "class Node {\n"
                          "    weak peer: Node?\n"
                          "    owner: Node?\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, uri, content, 1);
    ASSERT(doc != NULL);

    // The publish path parses before it diagnoses; do the same here so the
    // walk has an AST to find the field declarations in.
    xlsp_parse_document(doc, server);

    XrJsonValue *diagnostics = xjson_new_array();
    xlsp_cycle_report_refresh(server);
    xlsp_cycle_report_diagnostics(doc, diagnostics);

    // The report predates the fix: an annotated field is no longer a candidate.
    ASSERT_EQ(xjson_array_len(diagnostics), 1);
    ASSERT(strstr(xjson_get_string(xjson_array_get(diagnostics, 0), "message"), "`Node.owner`"));

    xjson_free(diagnostics);
    remove_sidecar();
    xlsp_server_free(server);
}

TEST(code_action_cycle_offers_weak_without_default_or_batch) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *uri = "file:///cycle_action.xr";
    const char *content = "class Node {\n"
                          "    peer: Node?\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, uri, content, 1);
    ASSERT(doc != NULL);

    XrJsonValue *params = make_code_action_params(
        uri, 1, 4, 8,
        XLSP_CYCLE_DIAG_PREFIX
        "`Node.peer` is on a cycle that kept 2 object(s) / 112 bytes alive past teardown.\n"
        "Marking this field `weak` breaks the cycle, and asserts that Node does NOT own the Node "
        "it points at -- the field reads as null once the target is gone.");
    XrJsonValue *actions = xlsp_handle_code_action(server, params);
    ASSERT(actions != NULL);

    XrJsonValue *action = find_action_with_title(actions, "Mark `peer` weak");
    ASSERT(action != NULL);

    // The title states the ownership claim, so the choice can be made before
    // clicking (247 H.3).
    ASSERT(strstr(xjson_get_string(action, "title"), "does not own"));

    // The edit inserts the modifier ahead of the field name.
    XrJsonValue *edits =
        xjson_get_array(xjson_get_object(xjson_get_object(action, "edit"), "changes"), uri);
    ASSERT(edits != NULL);
    ASSERT_EQ(xjson_array_len(edits), 1);
    XrJsonValue *text_edit = xjson_array_get(edits, 0);
    ASSERT_STR_EQ(xjson_get_string(text_edit, "newText"), "weak ");
    XrJsonValue *er = xjson_get_object(text_edit, "range");
    ASSERT_EQ(xjson_get_int(xjson_get_object(er, "start"), "character"), 4);
    // An insertion, not a replacement: nothing existing is overwritten.
    ASSERT_EQ(xjson_get_int(xjson_get_object(er, "end"), "character"), 4);

    // 247 H.3, asserted rather than described: no action may be preferred, and
    // none may claim to fix more than the one edge it names. Choosing the wrong
    // edge does not leak -- it nulls a field that was still being read.
    for (int i = 0; i < xjson_array_len(actions); i++) {
        XrJsonValue *a = xjson_array_get(actions, i);
        ASSERT(xjson_get(a, "isPreferred") == NULL);
        const char *title = xjson_get_string(a, "title");
        ASSERT(title == NULL || (!strstr(title, "all") && !strstr(title, "All")));
    }

    xjson_free(actions);
    xjson_free(params);
    xlsp_server_free(server);
}

TEST(code_action_closure_cycle_offers_defer) {
    XrLspServer *server = xlsp_server_new();
    ASSERT(server != NULL);

    const char *uri = "file:///closure_cycle.xr";
    const char *content = "fn wire(b: Button) {\n"
                          "    b.onClick = fn() { print(b.label) }\n"
                          "}\n";
    XrLspDocument *doc = xlsp_document_open(server, uri, content, 1);
    ASSERT(doc != NULL);

    XrJsonValue *params = make_code_action_params(
        uri, 1, 4, 13,
        "closure cycle: this closure captures 'b' and is stored into 'b.onClick'\n"
        "Xray does not reclaim reference cycles, and `weak` is a field modifier that cannot break "
        "a capture edge.\n"
        "hint: clear the field when the scope ends -- defer b.onClick = null -- or pass 'b' as a "
        "parameter instead of capturing it");
    XrJsonValue *actions = xlsp_handle_code_action(server, params);
    ASSERT(actions != NULL);

    XrJsonValue *action = find_action_with_title(actions, "defer b.onClick = null");
    ASSERT(action != NULL);

    XrJsonValue *edits =
        xjson_get_array(xjson_get_object(xjson_get_object(action, "edit"), "changes"), uri);
    ASSERT(edits != NULL);
    ASSERT_EQ(xjson_array_len(edits), 1);
    XrJsonValue *text_edit = xjson_array_get(edits, 0);
    // Indentation is carried over from the assignment, so the result reads as
    // hand-written code rather than something to clean up after.
    ASSERT_STR_EQ(xjson_get_string(text_edit, "newText"), "    defer b.onClick = null\n");
    XrJsonValue *start = xjson_get_object(xjson_get_object(text_edit, "range"), "start");
    ASSERT_EQ(xjson_get_int(start, "line"), 2);
    ASSERT_EQ(xjson_get_int(start, "character"), 0);

    xjson_free(actions);
    xjson_free(params);
    xlsp_server_free(server);
}

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
    RUN_TEST(completion_inferred_int_members);
    RUN_TEST(contextual_u32_literal_preserves_completion_and_hover_type);
    RUN_TEST(completion_enum_static_variants_descriptor);
    RUN_TEST(completion_enum_descriptor_properties);
    RUN_TEST(completion_enum_iteration_variable_is_descriptor);
    RUN_TEST(hover_enum_descriptor_keeps_precise_type);
    RUN_TEST(completion_u8_array_registry_methods);
    RUN_TEST(completion_uint8_array_uses_canonical_byte_docs);
    RUN_TEST(completion_int_array_excludes_u8_registry_methods);
    RUN_TEST(completion_u8_slice_registry_methods);
    RUN_TEST(hover_u8_array_registry_method);
    RUN_TEST(hover_deprecated_message_roundtrip);
    RUN_TEST(signature_help_u8_array_registry_method);
    RUN_TEST(builtin_generic_array_uses_error_placeholder);
    RUN_TEST(global_type_query_builtins_use_canonical_names);
    RUN_TEST(completion_public_attributes_use_registry);
    RUN_TEST(exact_integer_bit_builtins_use_receiver_specialized_registry);
    RUN_TEST(param_mode_user_function_lsp_display);
    RUN_TEST(param_mode_semantic_tokens_mark_modes_and_call_access);
    RUN_TEST(block_quoted_literals_preserve_lsp_positions_and_semantic_boundaries);
    RUN_TEST(block_import_path_uses_shared_quoted_literal_decoder);
    RUN_TEST(param_mode_inlay_hints_describe_modes);
    RUN_TEST(throw_effect_inlay_hints_show_inferred_result);

    printf("\nCode action concurrency quick-fix tests:\n");
    RUN_TEST(code_action_payload_enum_iteration_to_variants);
    RUN_TEST(code_action_go_capture_has_no_keyword_rewrite);

    printf("\nReference-cycle code actions (task 247 phase H):\n");
    RUN_TEST(cycle_report_diagnoses_every_candidate_field);
    RUN_TEST(cycle_report_skips_already_weak_field);
    RUN_TEST(code_action_cycle_offers_weak_without_default_or_batch);
    RUN_TEST(code_action_closure_cycle_offers_defer);

    printf("\n=== Results: %d passed, %d failed ===\n\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
