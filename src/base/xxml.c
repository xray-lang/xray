/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxml.c - Pure C XML parser implementation (no runtime dependency)
 *
 * KEY CONCEPT:
 *   Single-pass O(n) state machine parser with SIMD-accelerated text
 *   scanning. Entity decoding supports full Unicode range (UTF-8 output).
 *   All buffers use xr_malloc, not GC.
 */

#include "xxml.h"
#include "xmalloc.h"
#include "xchecks.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

/* Max nesting depth — matches the stdlib-wide cap. */
#define XR_XML_MAX_DEPTH 64

/* ========================================================================
 * Configuration
 * ======================================================================== */

void xr_xml_parse_config_init(XrXmlParseConfig *config) {
    if (!config)
        return;
    config->preserve_whitespace = false;
    config->preserve_comments = false;
    config->preserve_cdata = true;
    config->validate_entities = true;
}

void xr_xml_write_config_init(XrXmlWriteConfig *config) {
    if (!config)
        return;
    config->indent = 0;
    config->declaration = true;
    memcpy(config->encoding, "UTF-8", 6);
}

/* ========================================================================
 * Node creation / manipulation
 * ======================================================================== */

static XrXmlNode *xml_node_alloc(XrXmlNodeType type) {
    XrXmlNode *node = (XrXmlNode *) xr_calloc(1, sizeof(XrXmlNode));
    if (!node)
        return NULL;
    node->type = type;
    return node;
}

XrXmlDocument *xr_xml_document_new(void) {
    XrXmlDocument *doc = (XrXmlDocument *) xr_calloc(1, sizeof(XrXmlDocument));
    if (!doc)
        return NULL;
    memcpy(doc->version, "1.0", 4);
    memcpy(doc->encoding, "UTF-8", 6);
    return doc;
}

XrXmlNode *xr_xml_element_new(const char *tag, size_t tag_len) {
    XrXmlNode *node = xml_node_alloc(XR_XML_ELEMENT);
    if (!node)
        return NULL;
    node->tag = xr_strndup(tag, tag_len);
    node->tag_len = tag_len;
    return node;
}

XrXmlNode *xr_xml_text_new(const char *text, size_t len) {
    XrXmlNode *node = xml_node_alloc(XR_XML_TEXT);
    if (!node)
        return NULL;
    node->content = xr_strndup(text, len);
    node->content_len = len;
    return node;
}

XrXmlNode *xr_xml_comment_new(const char *text, size_t len) {
    XrXmlNode *node = xml_node_alloc(XR_XML_COMMENT);
    if (!node)
        return NULL;
    node->content = xr_strndup(text, len);
    node->content_len = len;
    return node;
}

XrXmlNode *xr_xml_cdata_new(const char *text, size_t len) {
    XrXmlNode *node = xml_node_alloc(XR_XML_CDATA);
    if (!node)
        return NULL;
    node->content = xr_strndup(text, len);
    node->content_len = len;
    return node;
}

XrXmlNode *xr_xml_pi_new(const char *text, size_t len) {
    XrXmlNode *node = xml_node_alloc(XR_XML_PI);
    if (!node)
        return NULL;
    node->content = xr_strndup(text, len);
    node->content_len = len;
    return node;
}

void xr_xml_node_append_child(XrXmlNode *parent, XrXmlNode *child) {
    if (!parent || !child)
        return;

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

void xr_xml_node_set_attr(XrXmlNode *node, const char *name, size_t name_len, const char *value,
                          size_t value_len) {
    if (!node || node->type != XR_XML_ELEMENT)
        return;
    if (name_len == 0 || !name)
        return;

    /* Duplicate-name check: linear scan, prefix-byte guard */
    char n0 = name[0];
    for (int i = 0; i < node->attr_count; i++) {
        XrXmlAttr *a = &node->attrs[i];
        if (a->name_len == name_len && a->name && a->name[0] == n0 &&
            (name_len == 1 || memcmp(a->name + 1, name + 1, name_len - 1) == 0)) {
            xr_free(a->value);
            a->value = xr_strndup(value, value_len);
            a->value_len = value_len;
            return;
        }
    }

    /* Grow array if needed */
    if (node->attr_count >= node->attr_cap) {
        int new_cap = node->attr_cap ? node->attr_cap * 2 : 4;
        XR_REALLOC_OR_ABORT(node->attrs, (size_t) new_cap * sizeof(XrXmlAttr),
                            "xml attr array grow");
        node->attr_cap = new_cap;
    }

    XrXmlAttr *a = &node->attrs[node->attr_count++];
    a->name = xr_strndup(name, name_len);
    a->name_len = name_len;
    a->value = xr_strndup(value, value_len);
    a->value_len = value_len;
}

/* ========================================================================
 * Cleanup
 * ======================================================================== */

void xr_xml_node_free(XrXmlNode *node) {
    if (!node)
        return;

    XrXmlNode *child = node->first_child;
    while (child) {
        XrXmlNode *next = child->next_sibling;
        xr_xml_node_free(child);
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

void xr_xml_document_free(XrXmlDocument *doc) {
    if (!doc)
        return;
    xr_xml_node_free(doc->root);
    xr_free(doc);
}

void xr_xml_free_result(XrXmlParseResult *result) {
    if (!result)
        return;
    xr_xml_document_free(result->doc);
    xr_free(result->errors);
    result->doc = NULL;
    result->errors = NULL;
    result->error_count = 0;
}

/* ========================================================================
 * Parser internals
 * ======================================================================== */

typedef enum {
    XXML_STATE_TEXT,
    XXML_STATE_TAG_START,
    XXML_STATE_TAG_NAME,
    XXML_STATE_TAG_SPACE,
    XXML_STATE_ATTR_NAME,
    XXML_STATE_ATTR_EQ,
    XXML_STATE_ATTR_VALUE,
    XXML_STATE_TAG_CLOSE,
    XXML_STATE_END_TAG,
    XXML_STATE_COMMENT,
    XXML_STATE_CDATA,
    XXML_STATE_PI,
    XXML_STATE_DOCTYPE,
    XXML_STATE_ERROR
} XxmlState;

typedef struct {
    const char *data;
    size_t len;
    size_t pos;

    int line;
    int column;

    XxmlState state;

    XrXmlDocument *doc;
    XrXmlNode *current;
    int depth;

    /* Temporary buffers (xr_malloc'd) */
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

    XrXmlParseConfig config;

    /* Error accumulator */
    XrXmlError *errors;
    int error_count;
    int error_cap;
} XxmlParser;

/* ========== Helper macros ========== */

#define PEEK() (p->pos < p->len ? p->data[p->pos] : '\0')
#define AT_END() (p->pos >= p->len)

static inline void parser_advance(XxmlParser *p) {
    if (p->pos < p->len && p->data[p->pos] == '\n') {
        p->line++;
        p->column = 1;
    } else {
        p->column++;
    }
    p->pos++;
}

#define ADVANCE() parser_advance(p)

/* ========== Buffer operations ========== */

static void ensure_buf_cap(char **buf, size_t *cap, size_t needed) {
    if (*cap >= needed)
        return;
    size_t new_cap = *cap ? *cap * 2 : 64;
    while (new_cap < needed)
        new_cap *= 2;
    XR_REALLOC_OR_ABORT(*buf, new_cap, "xml parser buffer grow");
    *cap = new_cap;
}

static void buf_append_char(char **buf, size_t *len, size_t *cap, char c) {
    ensure_buf_cap(buf, cap, *len + 2);
    (*buf)[(*len)++] = c;
}

/* ========== Error handling (pure C) ========== */

static void add_error(XxmlParser *p, const char *type, const char *msg) {
    if (p->error_count >= p->error_cap) {
        int new_cap = p->error_cap ? p->error_cap * 2 : 4;
        XR_REALLOC_OR_ABORT(p->errors, (size_t) new_cap * sizeof(XrXmlError),
                            "xml error list grow");
        p->error_cap = new_cap;
    }
    XrXmlError *e = &p->errors[p->error_count++];
    e->line = p->line;
    e->column = p->column;
    snprintf(e->type, sizeof(e->type), "%s", type);
    snprintf(e->message, sizeof(e->message), "%s", msg);
}

/* ========== Entity decoding ========== */

static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char) cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char) (0xC0 | (cp >> 6));
        out[1] = (char) (0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char) (0xE0 | (cp >> 12));
        out[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char) (0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char) (0xF0 | (cp >> 18));
        out[1] = (char) (0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char) (0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

static const struct {
    const char *name;
    uint8_t name_len;
    uint32_t cp;
} NAMED_ENTITIES[] = {
    /* XML 1.0 predefined */
    {"lt", 2, '<'},
    {"gt", 2, '>'},
    {"amp", 3, '&'},
    {"quot", 4, '"'},
    {"apos", 4, '\''},
    /* HTML5 common set */
    {"nbsp", 4, 0x00A0},
    {"copy", 4, 0x00A9},
    {"reg", 3, 0x00AE},
    {"trade", 5, 0x2122},
    {"hellip", 6, 0x2026},
    {"ndash", 5, 0x2013},
    {"mdash", 5, 0x2014},
    {"lsquo", 5, 0x2018},
    {"rsquo", 5, 0x2019},
    {"ldquo", 5, 0x201C},
    {"rdquo", 5, 0x201D},
    {"laquo", 5, 0x00AB},
    {"raquo", 5, 0x00BB},
    {"middot", 6, 0x00B7},
    {"bull", 4, 0x2022},
    {"sect", 4, 0x00A7},
    {"para", 4, 0x00B6},
    {"deg", 3, 0x00B0},
    {"plusmn", 6, 0x00B1},
    {"times", 5, 0x00D7},
    {"divide", 6, 0x00F7},
    {"euro", 4, 0x20AC},
    {"pound", 5, 0x00A3},
    {"yen", 3, 0x00A5},
    {"cent", 4, 0x00A2},
};

static int decode_entity(const char *s, size_t len, char *out, int *out_len) {
    *out_len = 0;

    for (size_t i = 0; i < sizeof(NAMED_ENTITIES) / sizeof(NAMED_ENTITIES[0]); i++) {
        uint8_t n = NAMED_ENTITIES[i].name_len;
        if (len == n && memcmp(s, NAMED_ENTITIES[i].name, n) == 0) {
            *out_len = utf8_encode(NAMED_ENTITIES[i].cp, out);
            return n;
        }
    }

    /* Numeric entity &#xxx; or &#xXXX; */
    if (len >= 2 && s[0] == '#') {
        int base = 10;
        int start = 1;
        if (s[1] == 'x' || s[1] == 'X') {
            base = 16;
            start = 2;
        }
        uint32_t val = 0;
        int i = start;
        while (i < (int) len) {
            char c = s[i];
            if (base == 10 && c >= '0' && c <= '9') {
                val = val * 10 + (uint32_t) (c - '0');
            } else if (base == 16) {
                if (c >= '0' && c <= '9')
                    val = val * 16 + (uint32_t) (c - '0');
                else if (c >= 'a' && c <= 'f')
                    val = val * 16 + (uint32_t) (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    val = val * 16 + (uint32_t) (c - 'A' + 10);
                else
                    break;
            } else {
                break;
            }
            if (val > 0x10FFFF)
                break;
            i++;
        }
        if (i > start && val > 0 && val <= 0x10FFFF) {
            *out_len = utf8_encode(val, out);
            return i;
        }
    }
    return 0;
}

static void decode_entities_inplace(XxmlParser *p, char *buf, size_t *len) {
    size_t src_len = *len;
    size_t r = 0, w = 0;
    const bool strict = p && p->config.validate_entities;

    while (r < src_len) {
        if (buf[r] == '&') {
            size_t j = r + 1;
            while (j < src_len && buf[j] != ';' && j - r < 12)
                j++;
            if (j < src_len && buf[j] == ';') {
                char decoded[4];
                int decoded_len;
                int consumed = decode_entity(buf + r + 1, j - r - 1, decoded, &decoded_len);
                if (consumed > 0 && decoded_len > 0) {
                    memcpy(buf + w, decoded, decoded_len);
                    w += decoded_len;
                    r = j + 1;
                    continue;
                }
                if (strict) {
                    char msg[96];
                    size_t name_len = j - (r + 1);
                    if (name_len > 64)
                        name_len = 64;
                    snprintf(msg, sizeof(msg), "Unknown entity reference '&%.*s;'", (int) name_len,
                             buf + r + 1);
                    add_error(p, "InvalidEntity", msg);
                }
            }
        }
        buf[w++] = buf[r++];
    }
    *len = w;
}

/* ========== Text / attribute flush ========== */

static void flush_text(XxmlParser *p) {
    if (p->text_len == 0)
        return;

    decode_entities_inplace(p, p->text_buf, &p->text_len);

    if (!p->config.preserve_whitespace) {
        bool all_ws = true;
        for (size_t i = 0; i < p->text_len; i++) {
            char c = p->text_buf[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                all_ws = false;
                break;
            }
        }
        if (all_ws) {
            p->text_len = 0;
            return;
        }
    }

    XrXmlNode *text = xr_xml_text_new(p->text_buf, p->text_len);
    if (text)
        xr_xml_node_append_child(p->current, text);
    p->text_len = 0;
}

static void flush_attr(XxmlParser *p) {
    char *val_buf = p->attr_value_buf ? p->attr_value_buf : (char *) "";
    size_t val_len = p->attr_value_len;

    if (val_len > 0 && p->attr_value_buf) {
        decode_entities_inplace(p, p->attr_value_buf, &val_len);
    }

    xr_xml_node_set_attr(p->current, p->attr_name_buf, p->attr_name_len, val_buf, val_len);
}

/* ========== Element open/close ========== */

static XrXmlNode *parser_open_element(XxmlParser *p) {
    if (p->depth >= XR_XML_MAX_DEPTH) {
        add_error(p, "MaxDepthExceeded", "Maximum nesting depth exceeded");
        p->state = XXML_STATE_ERROR;
        return NULL;
    }

    XrXmlNode *elem = xr_xml_element_new(p->tag_buf, p->tag_len);
    if (!elem)
        return NULL;
    if (p->current) {
        xr_xml_node_append_child(p->current, elem);
    } else {
        p->doc->root = elem;
    }
    p->current = elem;
    p->depth++;
    return elem;
}

static void parser_close_element(XxmlParser *p) {
    if (p->current && p->current->parent) {
        p->current = p->current->parent;
    } else {
        p->current = NULL;
    }
    if (p->depth > 0)
        p->depth--;
}

/* ========================================================================
 * State machine parser
 * ======================================================================== */

XrXmlParseResult xr_xml_parse(const char *data, size_t len, const XrXmlParseConfig *config) {
    XrXmlParseResult result = {0};

    if (!data)
        return result;

    XxmlParser parser_storage;
    XxmlParser *p = &parser_storage;
    memset(p, 0, sizeof(XxmlParser));

    p->data = data;
    p->len = len;
    p->line = 1;
    p->column = 1;
    p->state = XXML_STATE_TEXT;

    p->doc = xr_xml_document_new();
    if (!p->doc)
        return result;

    if (config) {
        p->config = *config;
    } else {
        xr_xml_parse_config_init(&p->config);
    }

    /* Skip BOM */
    if (p->len >= 3 && (unsigned char) p->data[0] == 0xEF && (unsigned char) p->data[1] == 0xBB &&
        (unsigned char) p->data[2] == 0xBF) {
        p->pos = 3;
    }

    while (!AT_END()) {
        char c = PEEK();

        switch (p->state) {
            case XXML_STATE_TEXT:
                if (c == '<') {
                    flush_text(p);
                    p->state = XXML_STATE_TAG_START;
                    ADVANCE();
                } else if (c == '\0') {
                    break;
                } else {
                    buf_append_char(&p->text_buf, &p->text_len, &p->text_cap, c);
                    ADVANCE();
                }
                break;

            case XXML_STATE_TAG_START:
                if (c == '/') {
                    p->state = XXML_STATE_END_TAG;
                    p->tag_len = 0;
                    ADVANCE();
                } else if (c == '!') {
                    ADVANCE();
                    if (p->pos + 1 < p->len && p->data[p->pos] == '-' &&
                        p->data[p->pos + 1] == '-') {
                        p->pos += 2;
                        p->column += 2;
                        p->state = XXML_STATE_COMMENT;
                        p->text_len = 0;
                    } else if (p->pos + 6 < p->len &&
                               strncmp(p->data + p->pos, "[CDATA[", 7) == 0) {
                        p->pos += 7;
                        p->column += 7;
                        p->state = XXML_STATE_CDATA;
                        p->text_len = 0;
                    } else if (p->pos + 6 < p->len &&
                               strncasecmp(p->data + p->pos, "DOCTYPE", 7) == 0) {
                        p->state = XXML_STATE_DOCTYPE;
                    } else {
                        add_error(p, "InvalidTag", "Invalid tag");
                        p->state = XXML_STATE_TEXT;
                    }
                } else if (c == '?') {
                    p->text_len = 0;
                    p->state = XXML_STATE_PI;
                    ADVANCE();
                } else if (isalpha((unsigned char) c) || c == '_') {
                    p->state = XXML_STATE_TAG_NAME;
                    p->tag_len = 0;
                    buf_append_char(&p->tag_buf, &p->tag_len, &p->tag_cap, c);
                    ADVANCE();
                } else {
                    add_error(p, "InvalidTag", "Invalid tag start");
                    p->state = XXML_STATE_TEXT;
                }
                break;

            case XXML_STATE_TAG_NAME:
                if (isalnum((unsigned char) c) || c == '_' || c == '-' || c == ':' || c == '.') {
                    buf_append_char(&p->tag_buf, &p->tag_len, &p->tag_cap, c);
                    ADVANCE();
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!parser_open_element(p))
                        goto done;
                    p->state = XXML_STATE_TAG_SPACE;
                    ADVANCE();
                } else if (c == '>') {
                    if (!parser_open_element(p))
                        goto done;
                    p->state = XXML_STATE_TEXT;
                    ADVANCE();
                } else if (c == '/') {
                    /* Self-closing */
                    XrXmlNode *elem = xr_xml_element_new(p->tag_buf, p->tag_len);
                    if (p->current) {
                        xr_xml_node_append_child(p->current, elem);
                    } else {
                        p->doc->root = elem;
                    }
                    p->state = XXML_STATE_TAG_CLOSE;
                    ADVANCE();
                } else {
                    add_error(p, "InvalidTag", "Invalid character in tag name");
                    p->state = XXML_STATE_TEXT;
                }
                break;

            case XXML_STATE_TAG_SPACE:
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else if (c == '>') {
                    p->state = XXML_STATE_TEXT;
                    ADVANCE();
                } else if (c == '/') {
                    parser_close_element(p);
                    p->state = XXML_STATE_TAG_CLOSE;
                    ADVANCE();
                } else if (isalpha((unsigned char) c) || c == '_') {
                    p->attr_name_len = 0;
                    buf_append_char(&p->attr_name_buf, &p->attr_name_len, &p->attr_name_cap, c);
                    p->state = XXML_STATE_ATTR_NAME;
                    ADVANCE();
                } else {
                    add_error(p, "InvalidAttr", "Invalid attribute");
                    p->state = XXML_STATE_TEXT;
                }
                break;

            case XXML_STATE_ATTR_NAME:
                if (isalnum((unsigned char) c) || c == '_' || c == '-' || c == ':') {
                    buf_append_char(&p->attr_name_buf, &p->attr_name_len, &p->attr_name_cap, c);
                    ADVANCE();
                } else if (c == '=') {
                    p->state = XXML_STATE_ATTR_EQ;
                    ADVANCE();
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else {
                    add_error(p, "InvalidAttr", "Invalid attribute name");
                    p->state = XXML_STATE_TAG_SPACE;
                }
                break;

            case XXML_STATE_ATTR_EQ:
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else if (c == '"' || c == '\'') {
                    p->quote_char = c;
                    p->attr_value_len = 0;
                    p->state = XXML_STATE_ATTR_VALUE;
                    ADVANCE();
                } else {
                    add_error(p, "InvalidAttr", "Expected quote after =");
                    p->state = XXML_STATE_TAG_SPACE;
                }
                break;

            case XXML_STATE_ATTR_VALUE:
                if (c == p->quote_char) {
                    flush_attr(p);
                    p->state = XXML_STATE_TAG_SPACE;
                    ADVANCE();
                } else {
                    buf_append_char(&p->attr_value_buf, &p->attr_value_len, &p->attr_value_cap, c);
                    ADVANCE();
                }
                break;

            case XXML_STATE_TAG_CLOSE:
                if (c == '>') {
                    p->state = XXML_STATE_TEXT;
                    ADVANCE();
                } else {
                    add_error(p, "InvalidTag", "Expected > after /");
                    p->state = XXML_STATE_TEXT;
                }
                break;

            case XXML_STATE_END_TAG:
                if (isalnum((unsigned char) c) || c == '_' || c == '-' || c == ':' || c == '.') {
                    buf_append_char(&p->tag_buf, &p->tag_len, &p->tag_cap, c);
                    ADVANCE();
                } else if (c == '>') {
                    if (p->current && p->current->tag) {
                        if (p->current->tag_len != p->tag_len ||
                            memcmp(p->current->tag, p->tag_buf, p->tag_len) != 0) {
                            add_error(p, "MismatchedTag", "Mismatched closing tag");
                        }
                    }
                    parser_close_element(p);
                    p->state = XXML_STATE_TEXT;
                    ADVANCE();
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else {
                    add_error(p, "InvalidTag", "Invalid end tag");
                    p->state = XXML_STATE_TEXT;
                }
                break;

            case XXML_STATE_COMMENT:
                if (c == '-' && p->pos + 1 < p->len && p->data[p->pos + 1] == '-') {
                    if (p->pos + 2 < p->len && p->data[p->pos + 2] == '>') {
                        if (p->config.preserve_comments && p->current) {
                            XrXmlNode *comment = xr_xml_comment_new(p->text_buf, p->text_len);
                            if (comment)
                                xr_xml_node_append_child(p->current, comment);
                        }
                        p->text_len = 0;
                        p->pos += 3;
                        p->column += 3;
                        p->state = XXML_STATE_TEXT;
                    } else {
                        buf_append_char(&p->text_buf, &p->text_len, &p->text_cap, c);
                        ADVANCE();
                    }
                } else {
                    buf_append_char(&p->text_buf, &p->text_len, &p->text_cap, c);
                    ADVANCE();
                }
                break;

            case XXML_STATE_CDATA:
                if (c == ']' && p->pos + 1 < p->len && p->data[p->pos + 1] == ']' &&
                    p->pos + 2 < p->len && p->data[p->pos + 2] == '>') {
                    if (p->current) {
                        if (p->config.preserve_cdata) {
                            XrXmlNode *cdata = xr_xml_cdata_new(p->text_buf, p->text_len);
                            if (cdata)
                                xr_xml_node_append_child(p->current, cdata);
                        } else {
                            XrXmlNode *text = xr_xml_text_new(p->text_buf, p->text_len);
                            if (text)
                                xr_xml_node_append_child(p->current, text);
                        }
                    }
                    p->text_len = 0;
                    p->pos += 3;
                    p->column += 3;
                    p->state = XXML_STATE_TEXT;
                } else {
                    buf_append_char(&p->text_buf, &p->text_len, &p->text_cap, c);
                    ADVANCE();
                }
                break;

            case XXML_STATE_PI:
                if (c == '?' && p->pos + 1 < p->len && p->data[p->pos + 1] == '>') {
                    if (p->text_len > 0 && p->current) {
                        XrXmlNode *pi = xr_xml_pi_new(p->text_buf, p->text_len);
                        if (pi)
                            xr_xml_node_append_child(p->current, pi);
                    }
                    p->text_len = 0;
                    p->pos += 2;
                    p->column += 2;
                    p->state = XXML_STATE_TEXT;
                } else {
                    buf_append_char(&p->text_buf, &p->text_len, &p->text_cap, c);
                    ADVANCE();
                }
                break;

            case XXML_STATE_DOCTYPE: {
                int bracket_depth = 0;
                while (!AT_END()) {
                    c = PEEK();
                    if (c == '[') {
                        bracket_depth++;
                    } else if (c == ']') {
                        bracket_depth--;
                    } else if (c == '>' && bracket_depth <= 0) {
                        ADVANCE();
                        p->state = XXML_STATE_TEXT;
                        break;
                    }
                    ADVANCE();
                }
                break;
            }

            case XXML_STATE_ERROR:
                goto done;
        }
    }

    flush_text(p);

done:
    result.doc = p->doc;
    result.errors = p->errors;
    result.error_count = p->error_count;

    /* Free parser temporary buffers (not the errors or doc — they transfer to result) */
    xr_free(p->tag_buf);
    xr_free(p->attr_name_buf);
    xr_free(p->attr_value_buf);
    xr_free(p->text_buf);

    return result;
}
