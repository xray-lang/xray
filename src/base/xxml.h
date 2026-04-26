/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxml.h - Pure C XML parser (no runtime dependency)
 *
 * KEY CONCEPT:
 *   Single-pass O(n) XML parser producing a malloc-based DOM tree.
 *   Node types, parse configuration, error reporting, and serialization
 *   config are all pure C — no XrayIsolate, no GC objects.
 *
 *   stdlib/xml bridges this to runtime XrValue objects via a thin
 *   dom_to_xrvalue converter, following the same pattern as xjson/xtoml.
 */

#ifndef XXML_H
#define XXML_H

#include <stddef.h>
#include <stdbool.h>
#include "xdefs.h"

/* ========== Node types ========== */

typedef enum {
    XR_XML_DOCUMENT,
    XR_XML_ELEMENT,
    XR_XML_TEXT,
    XR_XML_COMMENT,
    XR_XML_CDATA,
    XR_XML_PI
} XrXmlNodeType;

/* ========== Attribute ========== */

typedef struct {
    char *name;
    size_t name_len;
    char *value;
    size_t value_len;
} XrXmlAttr;

/* ========== XML node ========== */

typedef struct XrXmlNode XrXmlNode;

struct XrXmlNode {
    XrXmlNodeType type;

    /* Element: tag name (owned, xr_malloc'd) */
    char *tag;
    size_t tag_len;

    /* Element: attributes (owned array) */
    XrXmlAttr *attrs;
    int attr_count;
    int attr_cap;

    /* Text/comment/CDATA/PI content (owned, xr_malloc'd) */
    char *content;
    size_t content_len;

    /* Tree structure */
    XrXmlNode *parent;
    XrXmlNode *first_child;
    XrXmlNode *last_child;
    XrXmlNode *next_sibling;
    XrXmlNode *prev_sibling;
    int child_count;
};

/* ========== XML document ========== */

typedef struct {
    XrXmlNode *root;
    char version[8];   /* "1.0" */
    char encoding[32]; /* "UTF-8" */
} XrXmlDocument;

/* ========== Parse configuration ========== */

typedef struct {
    bool preserve_whitespace;
    bool preserve_comments;
    bool preserve_cdata;
    bool validate_entities;
} XrXmlParseConfig;

/* ========== Serialization configuration ========== */

typedef struct {
    int indent; /* Indent spaces, 0 = compact */
    bool declaration;
    char encoding[32];
} XrXmlWriteConfig;

/* ========== Error entry ========== */

typedef struct {
    int line;
    int column;
    char type[32];
    char message[128];
} XrXmlError;

/* ========== Parse result ========== */

typedef struct {
    XrXmlDocument *doc;
    XrXmlError *errors;
    int error_count;
} XrXmlParseResult;

/* ========== Config init ========== */

XR_FUNC void xr_xml_parse_config_init(XrXmlParseConfig *config);
XR_FUNC void xr_xml_write_config_init(XrXmlWriteConfig *config);

/* ========== Node creation (xr_malloc, caller must free) ========== */

XR_FUNC XrXmlDocument *xr_xml_document_new(void);
XR_FUNC XrXmlNode *xr_xml_element_new(const char *tag, size_t tag_len);
XR_FUNC XrXmlNode *xr_xml_text_new(const char *text, size_t len);
XR_FUNC XrXmlNode *xr_xml_comment_new(const char *text, size_t len);
XR_FUNC XrXmlNode *xr_xml_cdata_new(const char *text, size_t len);
XR_FUNC XrXmlNode *xr_xml_pi_new(const char *text, size_t len);

/* ========== Node manipulation ========== */

XR_FUNC void xr_xml_node_append_child(XrXmlNode *parent, XrXmlNode *child);
XR_FUNC void xr_xml_node_set_attr(XrXmlNode *node, const char *name, size_t name_len,
                                  const char *value, size_t value_len);

/* ========== Parsing ========== */

/*
 * Parse XML data into a DOM tree.
 * config may be NULL for defaults. Caller must free result with xr_xml_free_result().
 */
XR_FUNC XrXmlParseResult xr_xml_parse(const char *data, size_t len, const XrXmlParseConfig *config);

/* ========== Cleanup ========== */

XR_FUNC void xr_xml_node_free(XrXmlNode *node);
XR_FUNC void xr_xml_document_free(XrXmlDocument *doc);
XR_FUNC void xr_xml_free_result(XrXmlParseResult *result);

#endif  // XXML_H
