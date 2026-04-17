/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml_node.c - XML node implementation (malloc-based, no GC)
 */

#include "xml_node.h"
#include "../../src/base/xmalloc.h"
#include <string.h>

// ========== Configuration initialization ==========

void xml_parse_config_init(XmlParseConfig *config) {
    config->preserve_whitespace = false;
    config->preserve_comments = false;
    config->preserve_cdata = true;
    config->validate_entities = true;
}

void xml_write_config_init(XmlWriteConfig *config) {
    config->indent = 0;
    config->declaration = true;
    config->encoding = "UTF-8";
}

// ========== Internal helpers ==========
// xr_strndup now lives in base/xmalloc.h

static XmlNode* xml_node_alloc(XmlNodeType type) {
    XmlNode *node = (XmlNode*)xr_calloc(1, sizeof(XmlNode));
    if (!node) return NULL;
    node->type = type;
    return node;
}

// ========== Node creation ==========

XmlDocument* xml_document_new(void) {
    XmlDocument *doc = (XmlDocument*)xr_calloc(1, sizeof(XmlDocument));
    if (!doc) return NULL;
    memcpy(doc->version, "1.0", 4);
    memcpy(doc->encoding, "UTF-8", 6);
    return doc;
}

XmlNode* xml_element_new(const char *tag, size_t tag_len) {
    XmlNode *node = xml_node_alloc(XML_NODE_ELEMENT);
    node->tag = xr_strndup(tag, tag_len);
    node->tag_len = tag_len;
    return node;
}

XmlNode* xml_text_new(const char *text, size_t len) {
    XmlNode *node = xml_node_alloc(XML_NODE_TEXT);
    node->content = xr_strndup(text, len);
    node->content_len = len;
    return node;
}

XmlNode* xml_comment_new(const char *text, size_t len) {
    XmlNode *node = xml_node_alloc(XML_NODE_COMMENT);
    node->content = xr_strndup(text, len);
    node->content_len = len;
    return node;
}

XmlNode* xml_cdata_new(const char *text, size_t len) {
    XmlNode *node = xml_node_alloc(XML_NODE_CDATA);
    node->content = xr_strndup(text, len);
    node->content_len = len;
    return node;
}

// ========== Node operations ==========

void xml_node_append_child(XmlNode *parent, XmlNode *child) {
    if (!parent || !child) return;

    child->parent = parent;
    child->next_sibling = NULL;
    child->prev_sibling = parent->last_child;

    if (parent->last_child) {
        parent->last_child->next_sibling = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
    parent->child_count++;
}

void xml_node_set_attr(XmlNode *node,
                       const char *name, size_t name_len,
                       const char *value, size_t value_len) {
    if (!node || node->type != XML_NODE_ELEMENT) return;

    // Check for existing attribute with same name
    for (int i = 0; i < node->attr_count; i++) {
        if (node->attrs[i].name_len == name_len &&
            memcmp(node->attrs[i].name, name, name_len) == 0) {
            xr_free(node->attrs[i].value);
            node->attrs[i].value = xr_strndup(value, value_len);
            node->attrs[i].value_len = value_len;
            return;
        }
    }

    // Grow array if needed
    if (node->attr_count >= node->attr_cap) {
        int new_cap = node->attr_cap ? node->attr_cap * 2 : 4;
        XmlAttr *na = (XmlAttr*)xr_realloc(node->attrs, new_cap * sizeof(XmlAttr));
        if (!na) return;
        node->attrs = na;
        node->attr_cap = new_cap;
    }

    XmlAttr *a = &node->attrs[node->attr_count++];
    a->name = xr_strndup(name, name_len);
    a->name_len = name_len;
    a->value = xr_strndup(value, value_len);
    a->value_len = value_len;
}

// ========== Cleanup ==========

void xml_node_free(XmlNode *node) {
    if (!node) return;

    // Free children recursively
    XmlNode *child = node->first_child;
    while (child) {
        XmlNode *next = child->next_sibling;
        xml_node_free(child);
        child = next;
    }

    xr_free(node->tag);
    xr_free(node->content);

    for (int i = 0; i < node->attr_count; i++) {
        xr_free(node->attrs[i].name);
        xr_free(node->attrs[i].value);
    }
    xr_free(node->attrs);

    xr_free(node);
}

void xml_document_free(XmlDocument *doc) {
    if (!doc) return;
    xml_node_free(doc->root);
    xr_free(doc);
}
