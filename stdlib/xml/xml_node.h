/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml_node.h - XML node structure definition
 *
 * KEY CONCEPT:
 *   Temporary XML DOM nodes for parsing and building.
 *   Nodes use xr_malloc (not GC) and are freed after
 *   conversion to XrMap for xray script access.
 *
 * WHY THIS DESIGN:
 *   - XmlNode is a transient parse artifact, not a runtime object
 *   - Storing raw char* avoids GC dependencies during parsing
 *   - After node_to_map() conversion, the entire tree is freed
 */

#ifndef XR_STDLIB_XML_NODE_H
#define XR_STDLIB_XML_NODE_H

#include "../../src/base/xdefs.h"
#include <stddef.h>
#include <stdbool.h>

// ========== Node types ==========

typedef enum {
    XML_NODE_DOCUMENT,
    XML_NODE_ELEMENT,
    XML_NODE_TEXT,
    XML_NODE_COMMENT,
    XML_NODE_CDATA,
    XML_NODE_PI
} XmlNodeType;

// ========== Forward declarations ==========

typedef struct XmlNode XmlNode;
typedef struct XmlDocument XmlDocument;

// ========== Attribute ==========

typedef struct {
    char *name;
    size_t name_len;
    char *value;
    size_t value_len;
} XmlAttr;

// ========== XML node structure ==========

struct XmlNode {
    XmlNodeType type;

    // Element: tag name (owned, xr_malloc'd)
    char *tag;
    size_t tag_len;

    // Element: attributes (owned array)
    XmlAttr *attrs;
    int attr_count;
    int attr_cap;

    // Text/comment/CDATA content (owned, xr_malloc'd)
    char *content;
    size_t content_len;

    // Tree structure
    XmlNode *parent;
    XmlNode *first_child;
    XmlNode *last_child;
    XmlNode *next_sibling;
    XmlNode *prev_sibling;
    int child_count;
};

// ========== XML document structure ==========

struct XmlDocument {
    XmlNode *root;
    char version[8];     // "1.0"
    char encoding[32];   // "UTF-8"
};

// ========== Parse configuration ==========

typedef struct {
    bool preserve_whitespace;
    bool preserve_comments;
    bool preserve_cdata;
    bool validate_entities;
} XmlParseConfig;

// ========== Serialization configuration ==========

typedef struct {
    int indent;              // Indent spaces, 0 = compact
    bool declaration;
    char encoding[32];       // Encoding declaration (NUL-terminated, copied
                             // from caller to stay valid across GC)
} XmlWriteConfig;

// ========== Configuration ==========

XR_FUNC void xml_parse_config_init(XmlParseConfig *config);
XR_FUNC void xml_write_config_init(XmlWriteConfig *config);

// ========== Node creation (xr_malloc, caller must free) ==========

XR_FUNC XmlDocument* xml_document_new(void);
XR_FUNC XmlNode* xml_element_new(const char *tag, size_t tag_len);
XR_FUNC XmlNode* xml_text_new(const char *text, size_t len);
XR_FUNC XmlNode* xml_comment_new(const char *text, size_t len);
XR_FUNC XmlNode* xml_cdata_new(const char *text, size_t len);

// ========== Node manipulation ==========

XR_FUNC void xml_node_append_child(XmlNode *parent, XmlNode *child);
XR_FUNC void xml_node_set_attr(XmlNode *node,
                               const char *name, size_t name_len,
                               const char *value, size_t value_len);

// ========== Cleanup ==========

XR_FUNC void xml_node_free(XmlNode *node);
XR_FUNC void xml_document_free(XmlDocument *doc);

#endif
