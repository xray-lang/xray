/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml_parser.c - XML state machine parser implementation
 *
 * KEY CONCEPT:
 *   Single-pass O(n) parser with SIMD-accelerated text scanning.
 *   Entity decoding supports full Unicode range (UTF-8 output).
 *   All temporary buffers use xr_malloc, not GC.
 */

#include "xml_parser.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "xsimd.h"

static inline const char* xml_simd_scan_text(const char *s, size_t len) {
    const char chars[2] = {'<', '&'};
    return xr_simd_find_any(s, len, chars, 2);
}

// ========== External declarations ==========

extern XrValue xr_string_value(XrString *str);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue xr_value_from_map(XrMap *map);

// ========== Helper macros ==========

#define PEEK()    (parser->pos < parser->len ? parser->data[parser->pos] : '\0')
#define AT_END()  (parser->pos >= parser->len)

static inline void advance(XmlParser *parser) {
    if (parser->pos < parser->len && parser->data[parser->pos] == '\n') {
        parser->line++;
        parser->column = 1;
    } else {
        parser->column++;
    }
    parser->pos++;
}

#define ADVANCE() advance(parser)

// ========== Buffer operations ==========

static void ensure_buf_cap(char **buf, size_t *cap, size_t needed) {
    if (*cap >= needed) return;
    size_t new_cap = *cap ? *cap * 2 : 64;
    while (new_cap < needed) new_cap *= 2;
    *buf = (char*)xr_realloc(*buf, new_cap);
    *cap = new_cap;
}

static void buf_append_char(char **buf, size_t *len, size_t *cap, char c) {
    ensure_buf_cap(buf, cap, *len + 2);
    (*buf)[(*len)++] = c;
}

static void buf_append(char **buf, size_t *len, size_t *cap, const char *s, size_t slen) {
    ensure_buf_cap(buf, cap, *len + slen + 1);
    memcpy(*buf + *len, s, slen);
    *len += slen;
}

// ========== Error handling ==========

static void add_error(XmlParser *parser, XmlErrorType type, const char *msg) {
    XrMap *err = xr_map_new(xr_current_coro(parser->isolate));

    const char *type_str = "Unknown";
    switch (type) {
        case XML_ERROR_UNEXPECTED_EOF: type_str = "UnexpectedEOF"; break;
        case XML_ERROR_INVALID_TAG:    type_str = "InvalidTag"; break;
        case XML_ERROR_MISMATCHED_TAG: type_str = "MismatchedTag"; break;
        case XML_ERROR_INVALID_ATTR:   type_str = "InvalidAttr"; break;
        case XML_ERROR_INVALID_ENTITY: type_str = "InvalidEntity"; break;
        case XML_ERROR_INVALID_COMMENT:type_str = "InvalidComment"; break;
        case XML_ERROR_INVALID_CDATA:  type_str = "InvalidCData"; break;
        case XML_ERROR_MAX_DEPTH:      type_str = "MaxDepthExceeded"; break;
        default: break;
    }

    XrayIsolate *X = parser->isolate;
    XrValue key, val;

    key = xr_string_value(xr_string_intern(X, "type", 4, 0));
    val = xr_string_value(xr_string_intern(X, type_str, strlen(type_str), 0));
    xr_map_set(err, key, val);

    key = xr_string_value(xr_string_intern(X, "line", 4, 0));
    xr_map_set(err, key, xr_int(parser->line));

    key = xr_string_value(xr_string_intern(X, "column", 6, 0));
    xr_map_set(err, key, xr_int(parser->column));

    key = xr_string_value(xr_string_intern(X, "message", 7, 0));
    val = xr_string_value(xr_string_intern(X, msg, strlen(msg), 0));
    xr_map_set(err, key, val);

    xr_array_push(parser->errors, xr_value_from_map(err));
}

// ========== Entity decoding ==========

// Encode Unicode codepoint to UTF-8, return bytes written
static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

// Decode named/numeric entity after '&'. Returns consumed chars (excluding &;).
// out must have at least 4 bytes. out_len receives bytes written.
static int decode_entity(const char *s, size_t len, char *out, int *out_len) {
    *out_len = 0;

    // Named entities: portable byte comparison
    if (len >= 2 && s[0] == 'l' && s[1] == 't') {
        out[0] = '<'; *out_len = 1; return 2;
    }
    if (len >= 2 && s[0] == 'g' && s[1] == 't') {
        out[0] = '>'; *out_len = 1; return 2;
    }
    if (len >= 3 && s[0] == 'a' && s[1] == 'm' && s[2] == 'p') {
        out[0] = '&'; *out_len = 1; return 3;
    }
    if (len >= 4 && s[0] == 'q' && s[1] == 'u' && s[2] == 'o' && s[3] == 't') {
        out[0] = '"'; *out_len = 1; return 4;
    }
    if (len >= 4 && s[0] == 'a' && s[1] == 'p' && s[2] == 'o' && s[3] == 's') {
        out[0] = '\''; *out_len = 1; return 4;
    }

    // Numeric entity &#xxx; or &#xXXX;
    if (len >= 2 && s[0] == '#') {
        int base = 10;
        int start = 1;
        if (s[1] == 'x' || s[1] == 'X') {
            base = 16;
            start = 2;
        }
        uint32_t val = 0;
        int i = start;
        while (i < (int)len) {
            char c = s[i];
            if (base == 10 && c >= '0' && c <= '9') {
                val = val * 10 + (uint32_t)(c - '0');
            } else if (base == 16) {
                if (c >= '0' && c <= '9') val = val * 16 + (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') val = val * 16 + (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') val = val * 16 + (uint32_t)(c - 'A' + 10);
                else break;
            } else {
                break;
            }
            if (val > 0x10FFFF) break;
            i++;
        }
        if (i > start && val > 0 && val <= 0x10FFFF) {
            *out_len = utf8_encode(val, out);
            return i;
        }
    }
    return 0;
}

// In-place entity decoding for a buffer
static void decode_entities_inplace(char *buf, size_t *len) {
    size_t src_len = *len;
    size_t r = 0, w = 0;

    while (r < src_len) {
        if (buf[r] == '&') {
            size_t j = r + 1;
            while (j < src_len && buf[j] != ';' && j - r < 12) j++;
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
            }
        }
        buf[w++] = buf[r++];
    }
    *len = w;
}

// ========== Flush text node ==========

static void flush_text(XmlParser *parser) {
    if (parser->text_len == 0) return;

    decode_entities_inplace(parser->text_buf, &parser->text_len);

    if (!parser->config.preserve_whitespace) {
        bool all_ws = true;
        for (size_t i = 0; i < parser->text_len; i++) {
            char c = parser->text_buf[i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                all_ws = false;
                break;
            }
        }
        if (all_ws) {
            parser->text_len = 0;
            return;
        }
    }

    XmlNode *text = xml_text_new(parser->text_buf, parser->text_len);
    xml_node_append_child(parser->current, text);
    parser->text_len = 0;
}

// Decode entities in attribute value buffer and set on node
static void flush_attr(XmlParser *parser) {
    char *val_buf = parser->attr_value_buf ? parser->attr_value_buf : (char*)"";
    size_t val_len = parser->attr_value_len;

    if (val_len > 0 && parser->attr_value_buf) {
        decode_entities_inplace(parser->attr_value_buf, &val_len);
    }

    xml_node_set_attr(parser->current,
                      parser->attr_name_buf, parser->attr_name_len,
                      val_buf, val_len);
}

// ========== Initialization ==========

void xml_parser_init(XmlParser *parser, XrayIsolate *X,
                     const char *data, size_t len,
                     XmlParseConfig *config) {
    memset(parser, 0, sizeof(XmlParser));

    parser->isolate = X;
    parser->data = data;
    parser->len = len;
    parser->pos = 0;
    parser->line = 1;
    parser->column = 1;
    parser->state = XML_STATE_TEXT;
    parser->depth = 0;

    parser->doc = xml_document_new();
    parser->current = NULL;

    if (config) {
        parser->config = *config;
    } else {
        xml_parse_config_init(&parser->config);
    }

    parser->errors = xr_array_new(xr_current_coro(X));
}

void xml_parser_cleanup(XmlParser *parser) {
    xr_free(parser->tag_buf);
    xr_free(parser->attr_name_buf);
    xr_free(parser->attr_value_buf);
    xr_free(parser->text_buf);
}

// ========== Configuration parsing ==========

void xml_config_from_json(XrayIsolate *X, XmlParseConfig *config, XrJson *json) {
    xml_parse_config_init(config);
    if (!json) return;

    XrValue val;

    val = xr_json_get_by_key(X, json, "preserveWhitespace");
    if (XR_IS_BOOL(val)) config->preserve_whitespace = XR_TO_BOOL(val);

    val = xr_json_get_by_key(X, json, "preserveComments");
    if (XR_IS_BOOL(val)) config->preserve_comments = XR_TO_BOOL(val);

    val = xr_json_get_by_key(X, json, "preserveCData");
    if (XR_IS_BOOL(val)) config->preserve_cdata = XR_TO_BOOL(val);

    val = xr_json_get_by_key(X, json, "validateEntities");
    if (XR_IS_BOOL(val)) config->validate_entities = XR_TO_BOOL(val);
}

// ========== Helper: create element and push depth ==========

static XmlNode* parser_open_element(XmlParser *parser) {
    if (parser->depth >= XML_MAX_DEPTH) {
        add_error(parser, XML_ERROR_MAX_DEPTH, "Maximum nesting depth exceeded");
        parser->state = XML_STATE_ERROR;
        return NULL;
    }

    XmlNode *elem = xml_element_new(parser->tag_buf, parser->tag_len);
    if (parser->current) {
        xml_node_append_child(parser->current, elem);
    } else {
        parser->doc->root = elem;
    }
    parser->current = elem;
    parser->depth++;
    return elem;
}

static void parser_close_element(XmlParser *parser) {
    if (parser->current && parser->current->parent) {
        parser->current = parser->current->parent;
    } else {
        parser->current = NULL;
    }
    if (parser->depth > 0) parser->depth--;
}

// ========== State machine parsing ==========

XmlParseResult xml_parser_parse(XmlParser *parser) {
    XmlParseResult result = { parser->doc, parser->errors };

    // Skip BOM
    if (parser->len >= 3 &&
        (unsigned char)parser->data[0] == 0xEF &&
        (unsigned char)parser->data[1] == 0xBB &&
        (unsigned char)parser->data[2] == 0xBF) {
        parser->pos = 3;
    }

    while (!AT_END()) {
        char c = PEEK();

        switch (parser->state) {
            case XML_STATE_TEXT:
                // SIMD fast path
                if (parser->pos + 16 <= parser->len) {
                    size_t remaining = parser->len - parser->pos;
                    const char *found = xml_simd_scan_text(
                        parser->data + parser->pos, remaining);
                    size_t offset = found - (parser->data + parser->pos);
                    if (offset > 0) {
                        buf_append(&parser->text_buf, &parser->text_len,
                                   &parser->text_cap,
                                   parser->data + parser->pos, offset);
                        // Track newlines in skipped region
                        for (size_t k = 0; k < offset; k++) {
                            if (parser->data[parser->pos + k] == '\n') {
                                parser->line++;
                                parser->column = 1;
                            } else {
                                parser->column++;
                            }
                        }
                        parser->pos += offset;
                    }
                }
                c = PEEK();
                if (c == '<') {
                    flush_text(parser);
                    parser->state = XML_STATE_TAG_START;
                    ADVANCE();
                } else if (c == '\0') {
                    break;
                } else {
                    buf_append_char(&parser->text_buf, &parser->text_len,
                                    &parser->text_cap, c);
                    ADVANCE();
                }
                break;

            case XML_STATE_TAG_START:
                if (c == '/') {
                    parser->state = XML_STATE_END_TAG;
                    parser->tag_len = 0;
                    ADVANCE();
                } else if (c == '!') {
                    ADVANCE();
                    if (parser->pos + 1 < parser->len &&
                        parser->data[parser->pos] == '-' &&
                        parser->data[parser->pos + 1] == '-') {
                        parser->pos += 2;
                        parser->column += 2;
                        parser->state = XML_STATE_COMMENT;
                        parser->text_len = 0;
                    } else if (parser->pos + 6 < parser->len &&
                               strncmp(parser->data + parser->pos, "[CDATA[", 7) == 0) {
                        parser->pos += 7;
                        parser->column += 7;
                        parser->state = XML_STATE_CDATA;
                        parser->text_len = 0;
                    } else if (parser->pos + 6 < parser->len &&
                               strncasecmp(parser->data + parser->pos, "DOCTYPE", 7) == 0) {
                        parser->state = XML_STATE_DOCTYPE;
                    } else {
                        add_error(parser, XML_ERROR_INVALID_TAG, "Invalid tag");
                        parser->state = XML_STATE_TEXT;
                    }
                } else if (c == '?') {
                    parser->state = XML_STATE_PI;
                    ADVANCE();
                } else if (isalpha((unsigned char)c) || c == '_') {
                    parser->state = XML_STATE_TAG_NAME;
                    parser->tag_len = 0;
                    buf_append_char(&parser->tag_buf, &parser->tag_len,
                                    &parser->tag_cap, c);
                    ADVANCE();
                } else {
                    add_error(parser, XML_ERROR_INVALID_TAG, "Invalid tag start");
                    parser->state = XML_STATE_TEXT;
                }
                break;

            case XML_STATE_TAG_NAME:
                if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.') {
                    buf_append_char(&parser->tag_buf, &parser->tag_len,
                                    &parser->tag_cap, c);
                    ADVANCE();
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    if (!parser_open_element(parser)) goto done;
                    parser->state = XML_STATE_TAG_SPACE;
                    ADVANCE();
                } else if (c == '>') {
                    if (!parser_open_element(parser)) goto done;
                    parser->state = XML_STATE_TEXT;
                    ADVANCE();
                } else if (c == '/') {
                    // Self-closing: create element but don't set as current
                    XmlNode *elem = xml_element_new(parser->tag_buf, parser->tag_len);
                    if (parser->current) {
                        xml_node_append_child(parser->current, elem);
                    } else {
                        parser->doc->root = elem;
                    }
                    parser->state = XML_STATE_TAG_CLOSE;
                    ADVANCE();
                } else {
                    add_error(parser, XML_ERROR_INVALID_TAG, "Invalid character in tag name");
                    parser->state = XML_STATE_TEXT;
                }
                break;

            case XML_STATE_TAG_SPACE:
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else if (c == '>') {
                    parser->state = XML_STATE_TEXT;
                    ADVANCE();
                } else if (c == '/') {
                    // Self-close after attributes
                    parser_close_element(parser);
                    parser->state = XML_STATE_TAG_CLOSE;
                    ADVANCE();
                } else if (isalpha((unsigned char)c) || c == '_') {
                    parser->attr_name_len = 0;
                    buf_append_char(&parser->attr_name_buf, &parser->attr_name_len,
                                    &parser->attr_name_cap, c);
                    parser->state = XML_STATE_ATTR_NAME;
                    ADVANCE();
                } else {
                    add_error(parser, XML_ERROR_INVALID_ATTR, "Invalid attribute");
                    parser->state = XML_STATE_TEXT;
                }
                break;

            case XML_STATE_ATTR_NAME:
                if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':') {
                    buf_append_char(&parser->attr_name_buf, &parser->attr_name_len,
                                    &parser->attr_name_cap, c);
                    ADVANCE();
                } else if (c == '=') {
                    parser->state = XML_STATE_ATTR_EQ;
                    ADVANCE();
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else {
                    add_error(parser, XML_ERROR_INVALID_ATTR, "Invalid attribute name");
                    parser->state = XML_STATE_TAG_SPACE;
                }
                break;

            case XML_STATE_ATTR_EQ:
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else if (c == '"' || c == '\'') {
                    parser->quote_char = c;
                    parser->attr_value_len = 0;
                    parser->state = XML_STATE_ATTR_VALUE;
                    ADVANCE();
                } else {
                    add_error(parser, XML_ERROR_INVALID_ATTR, "Expected quote after =");
                    parser->state = XML_STATE_TAG_SPACE;
                }
                break;

            case XML_STATE_ATTR_VALUE:
                if (c == parser->quote_char) {
                    flush_attr(parser);
                    parser->state = XML_STATE_TAG_SPACE;
                    ADVANCE();
                } else {
                    buf_append_char(&parser->attr_value_buf, &parser->attr_value_len,
                                    &parser->attr_value_cap, c);
                    ADVANCE();
                }
                break;

            case XML_STATE_TAG_CLOSE:
                if (c == '>') {
                    parser->state = XML_STATE_TEXT;
                    ADVANCE();
                } else {
                    add_error(parser, XML_ERROR_INVALID_TAG, "Expected > after /");
                    parser->state = XML_STATE_TEXT;
                }
                break;

            case XML_STATE_END_TAG:
                if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == ':' || c == '.') {
                    buf_append_char(&parser->tag_buf, &parser->tag_len,
                                    &parser->tag_cap, c);
                    ADVANCE();
                } else if (c == '>') {
                    if (parser->current && parser->current->tag) {
                        if (parser->current->tag_len != parser->tag_len ||
                            memcmp(parser->current->tag, parser->tag_buf,
                                   parser->tag_len) != 0) {
                            add_error(parser, XML_ERROR_MISMATCHED_TAG,
                                      "Mismatched closing tag");
                        }
                    }
                    parser_close_element(parser);
                    parser->state = XML_STATE_TEXT;
                    ADVANCE();
                } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    ADVANCE();
                } else {
                    add_error(parser, XML_ERROR_INVALID_TAG, "Invalid end tag");
                    parser->state = XML_STATE_TEXT;
                }
                break;

            case XML_STATE_COMMENT:
                if (c == '-' && parser->pos + 1 < parser->len &&
                    parser->data[parser->pos + 1] == '-') {
                    if (parser->pos + 2 < parser->len &&
                        parser->data[parser->pos + 2] == '>') {
                        if (parser->config.preserve_comments && parser->current) {
                            XmlNode *comment = xml_comment_new(
                                parser->text_buf, parser->text_len);
                            xml_node_append_child(parser->current, comment);
                        }
                        parser->text_len = 0;
                        parser->pos += 3;
                        parser->column += 3;
                        parser->state = XML_STATE_TEXT;
                    } else {
                        buf_append_char(&parser->text_buf, &parser->text_len,
                                        &parser->text_cap, c);
                        ADVANCE();
                    }
                } else {
                    buf_append_char(&parser->text_buf, &parser->text_len,
                                    &parser->text_cap, c);
                    ADVANCE();
                }
                break;

            case XML_STATE_CDATA:
                if (c == ']' && parser->pos + 1 < parser->len &&
                    parser->data[parser->pos + 1] == ']' &&
                    parser->pos + 2 < parser->len &&
                    parser->data[parser->pos + 2] == '>') {
                    if (parser->current) {
                        if (parser->config.preserve_cdata) {
                            XmlNode *cdata = xml_cdata_new(
                                parser->text_buf, parser->text_len);
                            xml_node_append_child(parser->current, cdata);
                        } else {
                            XmlNode *text = xml_text_new(
                                parser->text_buf, parser->text_len);
                            xml_node_append_child(parser->current, text);
                        }
                    }
                    parser->text_len = 0;
                    parser->pos += 3;
                    parser->column += 3;
                    parser->state = XML_STATE_TEXT;
                } else {
                    buf_append_char(&parser->text_buf, &parser->text_len,
                                    &parser->text_cap, c);
                    ADVANCE();
                }
                break;

            case XML_STATE_PI:
                if (c == '?' && parser->pos + 1 < parser->len &&
                    parser->data[parser->pos + 1] == '>') {
                    parser->pos += 2;
                    parser->column += 2;
                    parser->state = XML_STATE_TEXT;
                } else {
                    ADVANCE();
                }
                break;

            case XML_STATE_DOCTYPE: {
                // Handle internal subset: <!DOCTYPE root [ ... ]>
                int bracket_depth = 0;
                while (!AT_END()) {
                    c = PEEK();
                    if (c == '[') {
                        bracket_depth++;
                    } else if (c == ']') {
                        bracket_depth--;
                    } else if (c == '>' && bracket_depth <= 0) {
                        ADVANCE();
                        parser->state = XML_STATE_TEXT;
                        break;
                    }
                    ADVANCE();
                }
                break;
            }

            case XML_STATE_ERROR:
                goto done;
        }
    }

    flush_text(parser);

done:
    return result;
}
