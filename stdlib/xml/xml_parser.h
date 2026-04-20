/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml_parser.h - XML state machine parser
 *
 * KEY CONCEPT:
 *   High-performance XML parser based on state machine.
 *   Features:
 *   - Single pass O(n) complexity
 *   - Entity decoding including Unicode numeric entities
 *   - Support for comments, CDATA, processing instructions
 */

#ifndef XR_STDLIB_XML_PARSER_H
#define XR_STDLIB_XML_PARSER_H

#include "../../src/base/xdefs.h"
#include "xml_node.h"

struct XrayIsolate;
struct XrArray;
struct XrJson;

// ========== Parser states ==========

typedef enum {
    XML_STATE_TEXT,
    XML_STATE_TAG_START,
    XML_STATE_TAG_NAME,
    XML_STATE_TAG_SPACE,
    XML_STATE_ATTR_NAME,
    XML_STATE_ATTR_EQ,
    XML_STATE_ATTR_VALUE,
    XML_STATE_TAG_CLOSE,
    XML_STATE_END_TAG,
    XML_STATE_COMMENT,
    XML_STATE_CDATA,
    XML_STATE_PI,
    XML_STATE_DOCTYPE,
    XML_STATE_ERROR
} XmlParseState;

// ========== Error types ==========

typedef enum {
    XML_ERROR_NONE,
    XML_ERROR_UNEXPECTED_EOF,
    XML_ERROR_INVALID_TAG,
    XML_ERROR_MISMATCHED_TAG,
    XML_ERROR_INVALID_ATTR,
    XML_ERROR_INVALID_ENTITY,
    XML_ERROR_INVALID_COMMENT,
    XML_ERROR_INVALID_CDATA,
    XML_ERROR_MAX_DEPTH
} XmlErrorType;

// ========== Parse result ==========

typedef struct {
    XmlDocument *doc;
    struct XrArray *errors;
} XmlParseResult;

// ========== Parser context ==========

typedef struct {
    struct XrayIsolate *isolate;

    const char *data;
    size_t len;
    size_t pos;

    int line;
    int column;

    XmlParseState state;

    XmlDocument *doc;
    XmlNode *current;
    int depth;

    // Temporary buffers (xr_malloc'd)
    char *tag_buf;
    size_t tag_len;
    size_t tag_cap;

    char *attr_name_buf;
    size_t attr_name_len;
    size_t attr_name_cap;

    char *attr_value_buf;
    size_t attr_value_len;
    size_t attr_value_cap;

    char *text_buf;
    size_t text_len;
    size_t text_cap;

    char quote_char;

    XmlParseConfig config;
    struct XrArray *errors;
} XmlParser;

// Use the stdlib-wide nesting cap so every parser (JSON/YAML/TOML/XML/CSV)
// agrees on what constitutes a pathologically deep document. Include
// `common_parser.h` before this file so XR_STDLIB_MAX_DEPTH is available.
#include "../common_parser.h"
#define XML_MAX_DEPTH XR_STDLIB_MAX_DEPTH

// ========== API ==========

XR_FUNC void xml_parser_init(XmlParser *parser, struct XrayIsolate *X,
                             const char *data, size_t len,
                             XmlParseConfig *config);

XR_FUNC XmlParseResult xml_parser_parse(XmlParser *parser);

XR_FUNC void xml_parser_cleanup(XmlParser *parser);

XR_FUNC void xml_config_from_json(struct XrayIsolate *X, XmlParseConfig *config,
                                  struct XrJson *json);

#endif
