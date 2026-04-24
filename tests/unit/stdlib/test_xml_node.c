/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xml_node.c - Unit tests for XML node creation and manipulation
 *
 * KEY CONCEPT:
 *   Tests XML document/element/text/comment/CDATA node creation,
 *   tree manipulation (append child, set attributes), and cleanup.
 */

#include "../test_framework.h"
#include <string.h>

#include "../../../src/base/xxml.h"

/* ========== Document ========== */

TEST(xml_document_new) {
    XrXmlDocument *doc = xr_xml_document_new();
    ASSERT_NOT_NULL(doc);
    // doc->root is NULL until populated by parser
    ASSERT_STR_EQ(doc->version, "1.0");
    ASSERT_STR_EQ(doc->encoding, "UTF-8");
    xr_xml_document_free(doc);
}

/* ========== Element ========== */

TEST(xml_element_new_basic) {
    XrXmlNode *node = xr_xml_element_new("div", 3);
    ASSERT_NOT_NULL(node);
    ASSERT_EQ_INT(node->type, XR_XML_ELEMENT);
    ASSERT_NOT_NULL(node->tag);
    ASSERT_EQ_INT((int)node->tag_len, 3);
    ASSERT_EQ_INT(strncmp(node->tag, "div", 3), 0);
    ASSERT_EQ_INT(node->child_count, 0);
    ASSERT_EQ_INT(node->attr_count, 0);
    xr_xml_node_free(node);
}

TEST(xml_element_new_long_tag) {
    const char *tag = "my-custom-element";
    XrXmlNode *node = xr_xml_element_new(tag, strlen(tag));
    ASSERT_NOT_NULL(node);
    ASSERT_EQ_INT((int)node->tag_len, (int)strlen(tag));
    xr_xml_node_free(node);
}

/* ========== Text ========== */

TEST(xml_text_new_basic) {
    XrXmlNode *node = xr_xml_text_new("Hello World", 11);
    ASSERT_NOT_NULL(node);
    ASSERT_EQ_INT(node->type, XR_XML_TEXT);
    ASSERT_NOT_NULL(node->content);
    ASSERT_EQ_INT((int)node->content_len, 11);
    xr_xml_node_free(node);
}

/* ========== Comment ========== */

TEST(xml_comment_new_basic) {
    XrXmlNode *node = xr_xml_comment_new("a comment", 9);
    ASSERT_NOT_NULL(node);
    ASSERT_EQ_INT(node->type, XR_XML_COMMENT);
    ASSERT_NOT_NULL(node->content);
    ASSERT_EQ_INT((int)node->content_len, 9);
    xr_xml_node_free(node);
}

/* ========== CDATA ========== */

TEST(xml_cdata_new_basic) {
    XrXmlNode *node = xr_xml_cdata_new("<raw>data</raw>", 15);
    ASSERT_NOT_NULL(node);
    ASSERT_EQ_INT(node->type, XR_XML_CDATA);
    ASSERT_EQ_INT((int)node->content_len, 15);
    xr_xml_node_free(node);
}

/* ========== Append Child ========== */

TEST(xml_append_child_single) {
    XrXmlNode *parent = xr_xml_element_new("div", 3);
    XrXmlNode *child = xr_xml_text_new("hello", 5);
    xr_xml_node_append_child(parent, child);

    ASSERT_EQ_INT(parent->child_count, 1);
    ASSERT_EQ_PTR(parent->first_child, child);
    ASSERT_EQ_PTR(parent->last_child, child);
    ASSERT_EQ_PTR(child->parent, parent);
    ASSERT_NULL(child->next_sibling);
    ASSERT_NULL(child->prev_sibling);

    xr_xml_node_free(parent);
}

TEST(xml_append_child_multiple) {
    XrXmlNode *parent = xr_xml_element_new("ul", 2);
    XrXmlNode *c1 = xr_xml_element_new("li", 2);
    XrXmlNode *c2 = xr_xml_element_new("li", 2);
    XrXmlNode *c3 = xr_xml_element_new("li", 2);

    xr_xml_node_append_child(parent, c1);
    xr_xml_node_append_child(parent, c2);
    xr_xml_node_append_child(parent, c3);

    ASSERT_EQ_INT(parent->child_count, 3);
    ASSERT_EQ_PTR(parent->first_child, c1);
    ASSERT_EQ_PTR(parent->last_child, c3);

    // Sibling chain
    ASSERT_EQ_PTR(c1->next_sibling, c2);
    ASSERT_EQ_PTR(c2->next_sibling, c3);
    ASSERT_NULL(c3->next_sibling);
    ASSERT_NULL(c1->prev_sibling);
    ASSERT_EQ_PTR(c2->prev_sibling, c1);
    ASSERT_EQ_PTR(c3->prev_sibling, c2);

    xr_xml_node_free(parent);
}

/* ========== Set Attribute ========== */

TEST(xml_set_attr_single) {
    XrXmlNode *node = xr_xml_element_new("input", 5);
    xr_xml_node_set_attr(node, "type", 4, "text", 4);

    ASSERT_EQ_INT(node->attr_count, 1);
    ASSERT_NOT_NULL(node->attrs);
    ASSERT_EQ_INT(strncmp(node->attrs[0].name, "type", 4), 0);
    ASSERT_EQ_INT(strncmp(node->attrs[0].value, "text", 4), 0);

    xr_xml_node_free(node);
}

TEST(xml_set_attr_multiple) {
    XrXmlNode *node = xr_xml_element_new("a", 1);
    xr_xml_node_set_attr(node, "href", 4, "/home", 5);
    xr_xml_node_set_attr(node, "class", 5, "link", 4);
    xr_xml_node_set_attr(node, "id", 2, "nav", 3);

    ASSERT_EQ_INT(node->attr_count, 3);

    xr_xml_node_free(node);
}

/* ========== Deep Tree ========== */

TEST(xml_deep_tree) {
    XrXmlNode *html = xr_xml_element_new("html", 4);
    XrXmlNode *body = xr_xml_element_new("body", 4);
    XrXmlNode *p = xr_xml_element_new("p", 1);
    XrXmlNode *text = xr_xml_text_new("Hello", 5);

    xr_xml_node_append_child(html, body);
    xr_xml_node_append_child(body, p);
    xr_xml_node_append_child(p, text);

    ASSERT_EQ_INT(html->child_count, 1);
    ASSERT_EQ_INT(body->child_count, 1);
    ASSERT_EQ_INT(p->child_count, 1);
    ASSERT_EQ_INT(text->child_count, 0);

    // Verify parent chain
    ASSERT_EQ_PTR(text->parent, p);
    ASSERT_EQ_PTR(p->parent, body);
    ASSERT_EQ_PTR(body->parent, html);

    xr_xml_node_free(html);
}

/* ========== Config Init ========== */

TEST(xml_parse_config_defaults) {
    XrXmlParseConfig config;
    xr_xml_parse_config_init(&config);
    // Defaults should be sensible
    ASSERT_FALSE(config.preserve_whitespace);
    ASSERT_FALSE(config.preserve_comments);
}

TEST(xml_write_config_defaults) {
    XrXmlWriteConfig config;
    xr_xml_write_config_init(&config);
    ASSERT_EQ_INT(config.indent, 0);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("XML - Document");
    RUN_TEST(xml_document_new);

    RUN_TEST_SUITE("XML - Element");
    RUN_TEST(xml_element_new_basic);
    RUN_TEST(xml_element_new_long_tag);

    RUN_TEST_SUITE("XML - Text/Comment/CDATA");
    RUN_TEST(xml_text_new_basic);
    RUN_TEST(xml_comment_new_basic);
    RUN_TEST(xml_cdata_new_basic);

    RUN_TEST_SUITE("XML - Append Child");
    RUN_TEST(xml_append_child_single);
    RUN_TEST(xml_append_child_multiple);

    RUN_TEST_SUITE("XML - Attributes");
    RUN_TEST(xml_set_attr_single);
    RUN_TEST(xml_set_attr_multiple);

    RUN_TEST_SUITE("XML - Deep Tree");
    RUN_TEST(xml_deep_tree);

    RUN_TEST_SUITE("XML - Config");
    RUN_TEST(xml_parse_config_defaults);
    RUN_TEST(xml_write_config_defaults);

TEST_MAIN_END()
