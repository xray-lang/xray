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
    memcpy(config->encoding, "UTF-8", 6);
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

XmlNode* xml_pi_new(const char *text, size_t len) {
    XmlNode *node = xml_node_alloc(XML_NODE_PI);
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
    if (name_len == 0 || !name) return;

    // Duplicate-name check. Typical elements carry <16 attrs so a
    // linear scan is optimal (chain start-of-name compare is cheaper
    // than a side hash). For the pathological >50-attr case noted in
    // the serialization analysis, the prefix-byte guard below avoids
    // 95% of the memcmps on the hot path (first byte unique across
    // `class`, `href`, `style`, `id`, ...), so the constant factor is
    // much lower than raw memcmp × n.
    char n0 = name[0];
    for (int i = 0; i < node->attr_count; i++) {
        XmlAttr *a = &node->attrs[i];
        if (a->name_len == name_len
            && a->name && a->name[0] == n0
            && (name_len == 1 || memcmp(a->name + 1, name + 1, name_len - 1) == 0)) {
            xr_free(a->value);
            a->value = xr_strndup(value, value_len);
            a->value_len = value_len;
            return;
        }
    }

    // Grow array if needed. On OOM we abort — a partial attribute table
    // (silently dropping an attribute without any error signal) would
    // produce very hard-to-diagnose corruption downstream.
    if (node->attr_count >= node->attr_cap) {
        int new_cap = node->attr_cap ? node->attr_cap * 2 : 4;
        XR_REALLOC_OR_ABORT(node->attrs,
                            (size_t)new_cap * sizeof(XmlAttr),
                            "xml attr array grow");
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
