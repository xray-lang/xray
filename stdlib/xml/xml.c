/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml.c - XML standard library implementation
 *
 * KEY CONCEPT:
 *   High-performance XML library based on state machine parsing.
 *   XmlNode trees are transient (xr_malloc) and freed after
 *   conversion to XrMap for xray script access.
 */

#include "xml.h"
#include "../../src/base/xxml.h"
#include "../common.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xarray.h"

// Forward declaration (defined in xjson.c)
extern struct XrArray *xr_json_keys(XrayIsolate *X, struct XrJson *json);
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/base/xmalloc.h"
#include "../common_writer.h"
#include "../common_io.h"
#include "../common_parser.h"
#include "../stdlib_cache.h"
#include <stdio.h>
#include <string.h>

// ========== External declarations ==========

extern XrValue xr_string_value(XrString *str);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue xr_value_from_map(XrMap *map);

// ========== Cached intern key strings ==========

// The raw struct was moved into stdlib_cache.h so that every xml.c
// binding shares a single per-isolate copy. We still expose XmlKeys as
// a local typedef for readability inside this file, pointing at the
// cached storage.
typedef XrStdlibXmlKeys XmlKeys;

// Ensure the isolate's XML key cache is populated (idempotent).
// Returns a pointer to the shared cache entry. Costs one branch after
// the first call; previously every binding did O(5) xr_string_intern()
// hashing on every invocation.
static XmlKeys *xml_keys_get(XrayIsolate *X) {
    XrStdlibCache *c = xr_stdlib_cache_get(X);
    XmlKeys *k = &c->xml_keys;
    if (k->ready)
        return k;
    k->type = xr_string_value(xr_string_intern(X, "type", 4, 0));
    k->tag = xr_string_value(xr_string_intern(X, "tag", 3, 0));
    k->attrs = xr_string_value(xr_string_intern(X, "attrs", 5, 0));
    k->children = xr_string_value(xr_string_intern(X, "children", 8, 0));
    k->text = xr_string_value(xr_string_intern(X, "text", 4, 0));
    k->namespaces = xr_string_value(xr_string_intern(X, "namespaces", 10, 0));
    k->str_element = xr_string_intern(X, "element", 7, 0);
    k->str_text = xr_string_intern(X, "text", 4, 0);
    k->str_comment = xr_string_intern(X, "comment", 7, 0);
    k->str_cdata = xr_string_intern(X, "cdata", 5, 0);
    k->str_document = xr_string_intern(X, "document", 8, 0);
    k->ready = true;
    return k;
}

// ========== Helper functions ==========

static void extract_config(XrayIsolate *X, XrValue config_val, XrXmlParseConfig *config) {
    xr_xml_parse_config_init(config);
    if (!xr_value_is_json(config_val))
        return;
    XrJson *json = xr_value_to_json(config_val);
    xrs_cfg_get_bool(X, json, "preserveWhitespace", &config->preserve_whitespace);
    xrs_cfg_get_bool(X, json, "preserveComments", &config->preserve_comments);
    xrs_cfg_get_bool(X, json, "preserveCData", &config->preserve_cdata);
    xrs_cfg_get_bool(X, json, "validateEntities", &config->validate_entities);
}

static void extract_write_config(XrayIsolate *X, XrValue config_val, XrXmlWriteConfig *config) {
    xr_xml_write_config_init(config);
    if (!xr_value_is_json(config_val))
        return;
    XrJson *json = xr_value_to_json(config_val);

    xrs_cfg_get_int(X, json, "indent", &config->indent);
    xrs_cfg_get_bool(X, json, "declaration", &config->declaration);
    xrs_cfg_get_fixed_str(X, json, "encoding", config->encoding, sizeof(config->encoding));
}

// ========== XmlNode to Map (with depth limit) ==========
//
// Aliased to the stdlib-wide cap (XR_STDLIB_MAX_DEPTH == 256) so every
// parse / stringify / serialize path uses the same nesting envelope.

#define NODE_TO_MAP_MAX_DEPTH XR_STDLIB_MAX_DEPTH

static XrValue node_to_map_r(XrayIsolate *X, XrXmlNode *node, XmlKeys *k, int depth) {
    if (!node || depth > NODE_TO_MAP_MAX_DEPTH)
        return xr_null();

    XrMap *map = xr_map_new(xr_current_coro(X));

    const char *type_str = "element";
    switch (node->type) {
        case XR_XML_ELEMENT:
            type_str = "element";
            break;
        case XR_XML_TEXT:
            type_str = "text";
            break;
        case XR_XML_COMMENT:
            type_str = "comment";
            break;
        case XR_XML_CDATA:
            type_str = "cdata";
            break;
        case XR_XML_DOCUMENT:
            type_str = "document";
            break;
        case XR_XML_PI:
            type_str = "pi";
            break;
        default:
            break;
    }
    xr_map_set(map, k->type, xr_string_value(xr_string_intern(X, type_str, strlen(type_str), 0)));

    if (node->type == XR_XML_ELEMENT) {
        if (node->tag) {
            xr_map_set(map, k->tag,
                       xr_string_value(xr_string_intern(X, node->tag, node->tag_len, 0)));
        }

        // Build attrs map from XrXmlAttr array. Split out xmlns
        // declarations into a dedicated namespaces Map.
        XrMap *attr_map = xr_map_new(xr_current_coro(X));
        XrMap *ns_map = NULL;
        for (int i = 0; i < node->attr_count; i++) {
            XrXmlAttr *a = &node->attrs[i];
            XrValue akey = xr_string_value(xr_string_intern(X, a->name, a->name_len, 0));
            XrValue aval = xr_string_value(xr_string_intern(X, a->value, a->value_len, 0));
            xr_map_set(attr_map, akey, aval);

            if (a->name_len >= 5 && memcmp(a->name, "xmlns", 5) == 0 &&
                (a->name_len == 5 || a->name[5] == ':')) {
                if (!ns_map) {
                    ns_map = xr_map_new(xr_current_coro(X));
                }
                const char *pfx = (a->name_len == 5) ? "" : a->name + 6;
                size_t pfxlen = (a->name_len == 5) ? 0 : a->name_len - 6;
                XrValue nkey = xr_string_value(xr_string_intern(X, pfx, pfxlen, 0));
                xr_map_set(ns_map, nkey, aval);
            }
        }
        xr_map_set(map, k->attrs, xr_value_from_map(attr_map));
        if (ns_map) {
            xr_map_set(map, k->namespaces, xr_value_from_map(ns_map));
        }

        XrArray *children = xr_array_new(xr_current_coro(X));
        for (XrXmlNode *child = node->first_child; child; child = child->next_sibling) {
            xr_array_push(children, node_to_map_r(X, child, k, depth + 1));
        }
        xr_map_set(map, k->children, xr_value_from_array(children));
    } else {
        if (node->content) {
            xr_map_set(map, k->text,
                       xr_string_value(xr_string_intern(X, node->content, node->content_len, 0)));
        }
    }

    return xr_value_from_map(map);
}

static XrValue node_to_map(XrayIsolate *X, XrXmlNode *node) {
    XmlKeys *k = xml_keys_get(X);
    return node_to_map_r(X, node, k, 0);
}

// ========== XML serialization ==========

typedef struct {
    XrSerWriter sw;  // shared byte buffer (sw.data / sw.len / sw.cap)
    int indent;
    int level;
} XmlWriter;

static inline void xw_init(XmlWriter *w, int indent, size_t hint) {
    xr_serw_init(&w->sw, hint > 256 ? hint : 256);
    w->indent = indent;
    w->level = 0;
}

static inline void xw_free(XmlWriter *w) {
    xr_serw_free(&w->sw);
}

static inline void xw_append(XmlWriter *w, const char *s, size_t n) {
    xr_serw_append(&w->sw, s, n);
}

static inline void xw_str(XmlWriter *w, const char *s) {
    xr_serw_str(&w->sw, s);
}

static inline void xw_char(XmlWriter *w, char c) {
    xr_serw_char(&w->sw, c);
}

static inline void xw_indent(XmlWriter *w) {
    xr_serw_indent(&w->sw, w->level, w->indent);
}

static inline void xw_newline(XmlWriter *w) {
    if (w->indent > 0)
        xw_char(w, '\n');
}

static void xw_escape_text(XmlWriter *w, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
            case '<':
                xw_str(w, "&lt;");
                break;
            case '>':
                xw_str(w, "&gt;");
                break;
            case '&':
                xw_str(w, "&amp;");
                break;
            default:
                xw_char(w, s[i]);
                break;
        }
    }
}

static void xw_escape_attr(XmlWriter *w, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
            case '<':
                xw_str(w, "&lt;");
                break;
            case '>':
                xw_str(w, "&gt;");
                break;
            case '&':
                xw_str(w, "&amp;");
                break;
            case '"':
                xw_str(w, "&quot;");
                break;
            default:
                xw_char(w, s[i]);
                break;
        }
    }
}

// Serialize Map node (with depth limit). Matches the stdlib-wide cap so
// round-tripping a freshly-parsed tree never fails at serialization.
#define SERIALIZE_MAX_DEPTH XR_STDLIB_MAX_DEPTH

static void serialize_map_node(XmlWriter *w, XrayIsolate *X, XrMap *map, XmlKeys *k, int depth) {
    if (!map || depth > SERIALIZE_MAX_DEPTH)
        return;

    XrValue type_val = xr_map_get(map, k->type, NULL);
    XrString *type_s = XR_IS_STRING(type_val) ? XR_TO_STRING(type_val) : k->str_element;

    if (type_s == k->str_element) {
        XrValue tag_val = xr_map_get(map, k->tag, NULL);
        if (!XR_IS_STRING(tag_val))
            return;
        XrString *tag = XR_TO_STRING(tag_val);

        xw_indent(w);
        xw_char(w, '<');
        xw_append(w, tag->data, tag->length);

        // Attributes: use xr_map_keys public API
        XrValue attrs_val = xr_map_get(map, k->attrs, NULL);
        if (XR_IS_MAP(attrs_val)) {
            XrMap *attrs = XR_TO_MAP(attrs_val);
            XrArray *keys = xr_map_keys(xr_current_coro(X), attrs);
            for (int i = 0; i < keys->length; i++) {
                XrValue akey = xr_array_get(keys, i);
                XrValue aval = xr_map_get(attrs, akey, NULL);
                if (XR_IS_STRING(akey) && XR_IS_STRING(aval)) {
                    XrString *ks = XR_TO_STRING(akey);
                    XrString *vs = XR_TO_STRING(aval);
                    xw_char(w, ' ');
                    xw_append(w, ks->data, ks->length);
                    xw_str(w, "=\"");
                    xw_escape_attr(w, vs->data, vs->length);
                    xw_char(w, '"');
                }
            }
        }

        // Children
        XrValue children_val = xr_map_get(map, k->children, NULL);
        if (XR_IS_ARRAY(children_val)) {
            XrArray *children = XR_TO_ARRAY(children_val);
            if (children->length > 0) {
                xw_char(w, '>');

                // Single text child: inline
                bool text_only = false;
                if (children->length == 1) {
                    XrValue first = xr_array_get(children, 0);
                    if (XR_IS_MAP(first)) {
                        XrValue ct = xr_map_get(XR_TO_MAP(first), k->type, NULL);
                        if (XR_IS_STRING(ct) && XR_TO_STRING(ct) == k->str_text) {
                            text_only = true;
                        }
                    }
                }

                if (text_only) {
                    XrMap *child_map = XR_TO_MAP(xr_array_get(children, 0));
                    XrValue tv = xr_map_get(child_map, k->text, NULL);
                    if (XR_IS_STRING(tv)) {
                        XrString *ts = XR_TO_STRING(tv);
                        xw_escape_text(w, ts->data, ts->length);
                    }
                } else {
                    xw_newline(w);
                    w->level++;
                    for (int i = 0; i < children->length; i++) {
                        XrValue child = xr_array_get(children, i);
                        if (XR_IS_MAP(child)) {
                            serialize_map_node(w, X, XR_TO_MAP(child), k, depth + 1);
                        }
                    }
                    w->level--;
                    xw_indent(w);
                }

                xw_str(w, "</");
                xw_append(w, tag->data, tag->length);
                xw_char(w, '>');
            } else {
                xw_str(w, "/>");
            }
        } else {
            xw_str(w, "/>");
        }
        if (depth > 0)
            xw_newline(w);
    } else if (type_s == k->str_text) {
        XrValue tv = xr_map_get(map, k->text, NULL);
        if (XR_IS_STRING(tv)) {
            XrString *ts = XR_TO_STRING(tv);
            xw_indent(w);
            xw_escape_text(w, ts->data, ts->length);
            xw_newline(w);
        }
    } else if (type_s == k->str_comment) {
        XrValue tv = xr_map_get(map, k->text, NULL);
        xw_indent(w);
        xw_str(w, "<!--");
        if (XR_IS_STRING(tv)) {
            XrString *ts = XR_TO_STRING(tv);
            xw_append(w, ts->data, ts->length);
        }
        xw_str(w, "-->");
        xw_newline(w);
    } else if (type_s == k->str_cdata) {
        XrValue tv = xr_map_get(map, k->text, NULL);
        xw_indent(w);
        xw_str(w, "<![CDATA[");
        if (XR_IS_STRING(tv)) {
            XrString *ts = XR_TO_STRING(tv);
            xw_append(w, ts->data, ts->length);
        }
        xw_str(w, "]]>");
        xw_newline(w);
    }
}

// ========== Module functions ==========

static XrValue xml_parse_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrString *str = XR_TO_STRING(args[0]);

    XrXmlParseConfig config;
    if (argc >= 2)
        extract_config(X, args[1], &config);
    else
        xr_xml_parse_config_init(&config);

    XrXmlParseResult result = xr_xml_parse(str->data, str->length, &config);

    XrValue ret = xr_null();
    if (result.doc && result.doc->root) {
        ret = node_to_map(X, result.doc->root);
    }
    xr_xml_free_result(&result);
    return ret;
}

static XrValue xml_parse_detailed(XrayIsolate *X, XrValue *args, int argc) {
    XmlKeys *k = xml_keys_get(X);

    if (argc < 1 || !XR_IS_STRING(args[0])) {
        XrMap *out = xr_map_new(xr_current_coro(X));
        XrValue kd = xr_string_value(xr_string_intern(X, "doc", 3, 0));
        xr_map_set(out, kd, xr_null());
        return xr_value_from_map(out);
    }

    XrString *str = XR_TO_STRING(args[0]);

    XrXmlParseConfig config;
    if (argc >= 2)
        extract_config(X, args[1], &config);
    else
        xr_xml_parse_config_init(&config);

    XrXmlParseResult result = xr_xml_parse(str->data, str->length, &config);

    XrMap *output = xr_map_new(xr_current_coro(X));
    XrValue key_doc = xr_string_value(xr_string_intern(X, "doc", 3, 0));
    if (result.doc && result.doc->root) {
        xr_map_set(output, key_doc, node_to_map_r(X, result.doc->root, k, 0));
    } else {
        xr_map_set(output, key_doc, xr_null());
    }

    // Convert C error array to XrArray of error maps
    XrArray *err_arr = xr_array_new(xr_current_coro(X));
    for (int i = 0; i < result.error_count; i++) {
        XrXmlError *e = &result.errors[i];
        xrs_error_push(X, err_arr, e->type, e->line, -1, e->column, e->message);
    }
    XrValue key_errors = xr_string_value(xr_string_intern(X, "errors", 6, 0));
    xr_map_set(output, key_errors, xr_value_from_array(err_arr));

    xr_xml_free_result(&result);

    return xr_value_from_map(output);
}

// parseFile. Synchronous; see stdlib/common_io.h for the P9 async plan.
static XrValue xml_parse_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrString *path = XR_TO_STRING(args[0]);

    char *content = NULL;
    size_t content_len = 0;
    if (!xrs_file_read_all_sync(path->data, &content, &content_len)) {
        return xr_null();
    }

    XrXmlParseConfig config;
    if (argc >= 2)
        extract_config(X, args[1], &config);
    else
        xr_xml_parse_config_init(&config);

    XrXmlParseResult result = xr_xml_parse(content, content_len, &config);
    xr_free(content);

    XrValue ret = xr_null();
    if (result.doc && result.doc->root) {
        ret = node_to_map(X, result.doc->root);
    }
    xr_xml_free_result(&result);
    return ret;
}

static XrValue xml_stringify_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_MAP(args[0])) {
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    }

    XrMap *node = XR_TO_MAP(args[0]);

    XrXmlWriteConfig config;
    if (argc >= 2)
        extract_write_config(X, args[1], &config);
    else
        xr_xml_write_config_init(&config);

    XmlWriter writer;
    xw_init(&writer, config.indent, 1024);

    if (config.declaration) {
        xw_str(&writer, "<?xml version=\"1.0\" encoding=\"");
        xw_str(&writer, config.encoding);
        xw_str(&writer, "\"?>");
        xw_newline(&writer);
    }

    XmlKeys *k = xml_keys_get(X);
    serialize_map_node(&writer, X, node, k, 0);

    XrString *result = xr_string_intern(X, writer.sw.data, writer.sw.len, 0);
    xw_free(&writer);
    return xr_string_value(result);
}

// writeFile. Synchronous; see stdlib/common_io.h for the P9 async plan.
static XrValue xml_write_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0]))
        return xr_bool(false);

    XrString *path = XR_TO_STRING(args[0]);

    XrValue str_args[2] = {args[1], argc >= 3 ? args[2] : xr_null()};
    XrValue xml_str = xml_stringify_fn(X, str_args, argc >= 3 ? 2 : 1);

    if (!XR_IS_STRING(xml_str))
        return xr_bool(false);
    XrString *str = XR_TO_STRING(xml_str);

    return xr_bool(xrs_file_write_all_sync(path->data, str->data, str->length));
}

// document(): create empty document Map
static XrValue xml_document_fn(XrayIsolate *X, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XmlKeys *k = xml_keys_get(X);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k->type, xr_string_value(xr_string_intern(X, "document", 8, 0)));
    xr_map_set(map, k->children, xr_value_from_array(xr_array_new(xr_current_coro(X))));
    return xr_value_from_map(map);
}

// element(tag) or element(tag, attrs)
static XrValue xml_element_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XrString *tag = XR_TO_STRING(args[0]);

    XmlKeys *k = xml_keys_get(X);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k->type, xr_string_value(xr_string_intern(X, "element", 7, 0)));
    xr_map_set(map, k->tag, xr_string_value(tag));

    // attrs: copy from Json argument or create empty
    if (argc >= 2 && xr_value_is_json(args[1])) {
        XrJson *json = xr_value_to_json(args[1]);
        XrMap *attr_map = xr_map_new(xr_current_coro(X));
        XrShape *shape = xr_json_shape(X, json);
        XrSymbolTable *st = X->symbol_table;
        for (uint16_t i = 0; i < shape->field_count; i++) {
            const char *fname = xr_symbol_get_name_in_table(st, shape->field_symbols[i]);
            if (fname) {
                XrValue jv = xr_json_get_field(json, i);
                if (XR_IS_STRING(jv)) {
                    XrValue fk = xr_string_value(xr_string_intern(X, fname, strlen(fname), 0));
                    xr_map_set(attr_map, fk, jv);
                }
            }
        }
        xr_map_set(map, k->attrs, xr_value_from_map(attr_map));
    } else {
        xr_map_set(map, k->attrs, xr_value_from_map(xr_map_new(xr_current_coro(X))));
    }

    xr_map_set(map, k->children, xr_value_from_array(xr_array_new(xr_current_coro(X))));
    return xr_value_from_map(map);
}

// text(content)
static XrValue xml_text_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XmlKeys *k = xml_keys_get(X);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k->type, xr_string_value(xr_string_intern(X, "text", 4, 0)));
    xr_map_set(map, k->text, args[0]);
    return xr_value_from_map(map);
}

// comment(content)
static XrValue xml_comment_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XmlKeys *k = xml_keys_get(X);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k->type, xr_string_value(xr_string_intern(X, "comment", 7, 0)));
    xr_map_set(map, k->text, args[0]);
    return xr_value_from_map(map);
}

// cdata(content)
static XrValue xml_cdata_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0]))
        return xr_null();

    XmlKeys *k = xml_keys_get(X);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k->type, xr_string_value(xr_string_intern(X, "cdata", 5, 0)));
    xr_map_set(map, k->text, args[0]);
    return xr_value_from_map(map);
}

// ========== Module loading ===========

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module xml

XR_DEFINE_BUILTIN(xml_parse_fn, "parse", "(data: string, options?: Json): Json?",
                  "Parse XML string")
XR_DEFINE_BUILTIN(xml_parse_detailed, "parseDetailed", "(data: string): Json?",
                  "Parse XML with details")
XR_DEFINE_BUILTIN(xml_parse_file, "parseFile", "(path: string): Json?", "Parse XML file")
XR_DEFINE_BUILTIN(xml_stringify_fn, "stringify", "(node: Json, options?: Json): string",
                  "Convert to XML string")
XR_DEFINE_BUILTIN(xml_write_file, "writeFile", "(path: string, node: Json): bool", "Write XML file")
XR_DEFINE_BUILTIN(xml_document_fn, "document", "(): Json", "Create XML document node")
XR_DEFINE_BUILTIN(xml_element_fn, "element", "(name: string, attrs?: Json): Json",
                  "Create XML element node")
XR_DEFINE_BUILTIN(xml_text_fn, "text", "(content: string): Json", "Create XML text node")
XR_DEFINE_BUILTIN(xml_comment_fn, "comment", "(content: string): Json", "Create XML comment node")
XR_DEFINE_BUILTIN(xml_cdata_fn, "cdata", "(content: string): Json", "Create XML CDATA node")

XrModule *xr_load_module_xml(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "xml");
    if (!mod)
        return NULL;

    XRS_EXPORT(mod, isolate, "parse", xml_parse_fn);
    XRS_EXPORT(mod, isolate, "parseDetailed", xml_parse_detailed);
    XRS_EXPORT(mod, isolate, "parseFile", xml_parse_file);
    XRS_EXPORT(mod, isolate, "stringify", xml_stringify_fn);
    XRS_EXPORT(mod, isolate, "writeFile", xml_write_file);
    XRS_EXPORT(mod, isolate, "document", xml_document_fn);
    XRS_EXPORT(mod, isolate, "element", xml_element_fn);
    XRS_EXPORT(mod, isolate, "text", xml_text_fn);
    XRS_EXPORT(mod, isolate, "comment", xml_comment_fn);
    XRS_EXPORT(mod, isolate, "cdata", xml_cdata_fn);

    mod->loaded = true;
    return mod;
}
