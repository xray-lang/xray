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
#include "xml_node.h"
#include "xml_parser.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xarray.h"

// Forward declaration (defined in xjson.c)
extern struct XrArray* xr_json_keys(XrayIsolate *X, struct XrJson *json);
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xstring.h"
#include "../../src/runtime/object/xjson.h"
#include "../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>

// ========== External declarations ==========

extern XrValue xr_string_value(XrString *str);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue xr_value_from_map(XrMap *map);

// ========== Cached intern key strings ==========

typedef struct {
    XrValue type;
    XrValue tag;
    XrValue attrs;
    XrValue children;
    XrValue text;
    // Interned type name strings for O(1) pointer comparison
    XrString *str_element;
    XrString *str_text;
    XrString *str_comment;
    XrString *str_cdata;
    XrString *str_document;
} XmlKeys;

static void xml_keys_init(XrayIsolate *X, XmlKeys *k) {
    k->type     = xr_string_value(xr_string_intern(X, "type", 4, 0));
    k->tag      = xr_string_value(xr_string_intern(X, "tag", 3, 0));
    k->attrs    = xr_string_value(xr_string_intern(X, "attrs", 5, 0));
    k->children = xr_string_value(xr_string_intern(X, "children", 8, 0));
    k->text     = xr_string_value(xr_string_intern(X, "text", 4, 0));
    k->str_element  = xr_string_intern(X, "element", 7, 0);
    k->str_text     = xr_string_intern(X, "text", 4, 0);
    k->str_comment  = xr_string_intern(X, "comment", 7, 0);
    k->str_cdata    = xr_string_intern(X, "cdata", 5, 0);
    k->str_document = xr_string_intern(X, "document", 8, 0);
}

// ========== Helper functions ==========

static void extract_config(XrayIsolate *X, XrValue config_val, XmlParseConfig *config) {
    xml_parse_config_init(config);
    if (xr_value_is_json(config_val)) {
        XrJson *json = xr_value_to_json(config_val);
        xml_config_from_json(X, config, json);
    }
}

static void extract_write_config(XrayIsolate *X, XrValue config_val, XmlWriteConfig *config) {
    xml_write_config_init(config);
    if (xr_value_is_json(config_val)) {
        XrJson *json = xr_value_to_json(config_val);

        XrValue val = xr_json_get_by_key(X, json, "indent");
        if (XR_IS_INT(val)) config->indent = (int)XR_TO_INT(val);

        val = xr_json_get_by_key(X, json, "declaration");
        if (XR_IS_BOOL(val)) config->declaration = XR_TO_BOOL(val);

        val = xr_json_get_by_key(X, json, "encoding");
        if (XR_IS_STRING(val)) config->encoding = XR_TO_STRING(val)->data;
    }
}

// ========== XmlNode to Map (with depth limit) ==========

#define NODE_TO_MAP_MAX_DEPTH 256

static XrValue node_to_map_r(XrayIsolate *X, XmlNode *node, XmlKeys *k, int depth) {
    if (!node || depth > NODE_TO_MAP_MAX_DEPTH) return xr_null();

    XrMap *map = xr_map_new(xr_current_coro(X));

    const char *type_str = "element";
    switch (node->type) {
        case XML_NODE_ELEMENT:  type_str = "element"; break;
        case XML_NODE_TEXT:     type_str = "text"; break;
        case XML_NODE_COMMENT:  type_str = "comment"; break;
        case XML_NODE_CDATA:    type_str = "cdata"; break;
        case XML_NODE_DOCUMENT: type_str = "document"; break;
        default: break;
    }
    xr_map_set(map, k->type,
               xr_string_value(xr_string_intern(X, type_str, strlen(type_str), 0)));

    if (node->type == XML_NODE_ELEMENT) {
        if (node->tag) {
            xr_map_set(map, k->tag,
                       xr_string_value(xr_string_intern(X, node->tag, node->tag_len, 0)));
        }

        // Build attrs map from XmlAttr array
        XrMap *attr_map = xr_map_new(xr_current_coro(X));
        for (int i = 0; i < node->attr_count; i++) {
            XmlAttr *a = &node->attrs[i];
            XrValue akey = xr_string_value(xr_string_intern(X, a->name, a->name_len, 0));
            XrValue aval = xr_string_value(xr_string_intern(X, a->value, a->value_len, 0));
            xr_map_set(attr_map, akey, aval);
        }
        xr_map_set(map, k->attrs, xr_value_from_map(attr_map));

        XrArray *children = xr_array_new(xr_current_coro(X));
        for (XmlNode *child = node->first_child; child; child = child->next_sibling) {
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

static XrValue node_to_map(XrayIsolate *X, XmlNode *node) {
    XmlKeys k;
    xml_keys_init(X, &k);
    return node_to_map_r(X, node, &k, 0);
}

// ========== XML serialization ==========

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int indent;
    int level;
} XmlWriter;

static void xw_init(XmlWriter *w, int indent, size_t hint) {
    w->cap = hint > 256 ? hint : 256;
    w->buf = (char*)xr_malloc(w->cap);
    if (!w->buf) { w->cap = 0; w->len = 0; return; }
    w->len = 0;
    w->indent = indent;
    w->level = 0;
}

static void xw_ensure(XmlWriter *w, size_t extra) {
    size_t needed = w->len + extra;
    if (needed < w->cap) return;
    while (w->cap <= needed) w->cap *= 2;
    char *nb = (char*)xr_realloc(w->buf, w->cap);
    if (!nb) return;
    w->buf = nb;
}

static void xw_append(XmlWriter *w, const char *s, size_t n) {
    xw_ensure(w, n);
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void xw_str(XmlWriter *w, const char *s) {
    xw_append(w, s, strlen(s));
}

static void xw_char(XmlWriter *w, char c) {
    xw_ensure(w, 1);
    w->buf[w->len++] = c;
}

static void xw_indent(XmlWriter *w) {
    if (w->indent <= 0) return;
    int n = w->level * w->indent;
    xw_ensure(w, n);
    memset(w->buf + w->len, ' ', n);
    w->len += n;
}

static void xw_newline(XmlWriter *w) {
    if (w->indent > 0) xw_char(w, '\n');
}

static void xw_escape_text(XmlWriter *w, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
            case '<': xw_str(w, "&lt;"); break;
            case '>': xw_str(w, "&gt;"); break;
            case '&': xw_str(w, "&amp;"); break;
            default:  xw_char(w, s[i]); break;
        }
    }
}

static void xw_escape_attr(XmlWriter *w, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        switch (s[i]) {
            case '<':  xw_str(w, "&lt;"); break;
            case '>':  xw_str(w, "&gt;"); break;
            case '&':  xw_str(w, "&amp;"); break;
            case '"':  xw_str(w, "&quot;"); break;
            default:   xw_char(w, s[i]); break;
        }
    }
}

// Serialize Map node (with depth limit)
#define SERIALIZE_MAX_DEPTH 256

static void serialize_map_node(XmlWriter *w, XrayIsolate *X, XrMap *map,
                               XmlKeys *k, int depth) {
    if (!map || depth > SERIALIZE_MAX_DEPTH) return;

    XrValue type_val = xr_map_get(map, k->type, NULL);
    XrString *type_s = XR_IS_STRING(type_val) ? XR_TO_STRING(type_val) : k->str_element;

    if (type_s == k->str_element) {
        XrValue tag_val = xr_map_get(map, k->tag, NULL);
        if (!XR_IS_STRING(tag_val)) return;
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
                        if (XR_IS_STRING(ct) &&
                            XR_TO_STRING(ct) == k->str_text) {
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
        if (depth > 0) xw_newline(w);
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
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_null();

    XrString *str = XR_TO_STRING(args[0]);

    XmlParseConfig config;
    if (argc >= 2) extract_config(X, args[1], &config);
    else xml_parse_config_init(&config);

    XmlParser parser;
    xml_parser_init(&parser, X, str->data, str->length, &config);
    XmlParseResult result = xml_parser_parse(&parser);
    xml_parser_cleanup(&parser);

    XrValue ret = xr_null();
    if (result.doc && result.doc->root) {
        ret = node_to_map(X, result.doc->root);
    }
    xml_document_free(result.doc);
    return ret;
}

static XrValue xml_parse_detailed(XrayIsolate *X, XrValue *args, int argc) {
    XmlKeys k;
    xml_keys_init(X, &k);

    if (argc < 1 || !XR_IS_STRING(args[0])) {
        XrMap *out = xr_map_new(xr_current_coro(X));
        XrValue kd = xr_string_value(xr_string_intern(X, "doc", 3, 0));
        xr_map_set(out, kd, xr_null());
        return xr_value_from_map(out);
    }

    XrString *str = XR_TO_STRING(args[0]);

    XmlParseConfig config;
    if (argc >= 2) extract_config(X, args[1], &config);
    else xml_parse_config_init(&config);

    XmlParser parser;
    xml_parser_init(&parser, X, str->data, str->length, &config);
    XmlParseResult result = xml_parser_parse(&parser);

    XrMap *output = xr_map_new(xr_current_coro(X));
    XrValue key_doc = xr_string_value(xr_string_intern(X, "doc", 3, 0));
    if (result.doc && result.doc->root) {
        xr_map_set(output, key_doc, node_to_map_r(X, result.doc->root, &k, 0));
    } else {
        xr_map_set(output, key_doc, xr_null());
    }

    XrValue key_errors = xr_string_value(xr_string_intern(X, "errors", 6, 0));
    xr_map_set(output, key_errors, xr_value_from_array(result.errors));

    xml_parser_cleanup(&parser);
    xml_document_free(result.doc);

    return xr_value_from_map(output);
}

static XrValue xml_parse_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_null();

    XrString *path = XR_TO_STRING(args[0]);

    FILE *f = fopen(path->data, "rb");
    if (!f) return xr_null();

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return xr_null(); }
    fseek(f, 0, SEEK_SET);

    char *content = (char*)xr_malloc((size_t)size + 1);
    if (!content) { fclose(f); return xr_null(); }
    size_t read_size = fread(content, 1, (size_t)size, f);
    content[read_size] = '\0';
    fclose(f);

    XmlParseConfig config;
    if (argc >= 2) extract_config(X, args[1], &config);
    else xml_parse_config_init(&config);

    XmlParser parser;
    xml_parser_init(&parser, X, content, read_size, &config);
    XmlParseResult result = xml_parser_parse(&parser);
    xml_parser_cleanup(&parser);
    xr_free(content);

    XrValue ret = xr_null();
    if (result.doc && result.doc->root) {
        ret = node_to_map(X, result.doc->root);
    }
    xml_document_free(result.doc);
    return ret;
}

static XrValue xml_stringify_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_MAP(args[0])) {
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    }

    XrMap *node = XR_TO_MAP(args[0]);

    XmlWriteConfig config;
    if (argc >= 2) extract_write_config(X, args[1], &config);
    else xml_write_config_init(&config);

    XmlWriter writer;
    xw_init(&writer, config.indent, 1024);

    if (config.declaration) {
        xw_str(&writer, "<?xml version=\"1.0\" encoding=\"");
        xw_str(&writer, config.encoding);
        xw_str(&writer, "\"?>");
        xw_newline(&writer);
    }

    XmlKeys k;
    xml_keys_init(X, &k);
    serialize_map_node(&writer, X, node, &k, 0);

    XrString *result = xr_string_intern(X, writer.buf, writer.len, 0);
    xr_free(writer.buf);
    return xr_string_value(result);
}

static XrValue xml_write_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0])) return xr_bool(false);

    XrString *path = XR_TO_STRING(args[0]);

    XrValue str_args[2] = { args[1], argc >= 3 ? args[2] : xr_null() };
    XrValue xml_str = xml_stringify_fn(X, str_args, argc >= 3 ? 2 : 1);

    if (!XR_IS_STRING(xml_str)) return xr_bool(false);
    XrString *str = XR_TO_STRING(xml_str);

    FILE *f = fopen(path->data, "wb");
    if (!f) return xr_bool(false);
    fwrite(str->data, 1, str->length, f);
    fclose(f);

    return xr_bool(true);
}

// document(): create empty document Map
static XrValue xml_document_fn(XrayIsolate *X, XrValue *args, int argc) {
    (void)args; (void)argc;

    XmlKeys k;
    xml_keys_init(X, &k);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k.type,
               xr_string_value(xr_string_intern(X, "document", 8, 0)));
    xr_map_set(map, k.children, xr_value_from_array(xr_array_new(xr_current_coro(X))));
    return xr_value_from_map(map);
}

// element(tag) or element(tag, attrs)
static XrValue xml_element_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_null();

    XrString *tag = XR_TO_STRING(args[0]);

    XmlKeys k;
    xml_keys_init(X, &k);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k.type,
               xr_string_value(xr_string_intern(X, "element", 7, 0)));
    xr_map_set(map, k.tag, xr_string_value(tag));

    // attrs: copy from Json argument or create empty
    if (argc >= 2 && xr_value_is_json(args[1])) {
        XrJson *json = xr_value_to_json(args[1]);
        XrMap *attr_map = xr_map_new(xr_current_coro(X));
        XrShape *shape = xr_json_shape(json);
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
        xr_map_set(map, k.attrs, xr_value_from_map(attr_map));
    } else {
        xr_map_set(map, k.attrs, xr_value_from_map(xr_map_new(xr_current_coro(X))));
    }

    xr_map_set(map, k.children, xr_value_from_array(xr_array_new(xr_current_coro(X))));
    return xr_value_from_map(map);
}

// text(content)
static XrValue xml_text_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_null();

    XmlKeys k;
    xml_keys_init(X, &k);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k.type,
               xr_string_value(xr_string_intern(X, "text", 4, 0)));
    xr_map_set(map, k.text, args[0]);
    return xr_value_from_map(map);
}

// comment(content)
static XrValue xml_comment_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_null();

    XmlKeys k;
    xml_keys_init(X, &k);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k.type,
               xr_string_value(xr_string_intern(X, "comment", 7, 0)));
    xr_map_set(map, k.text, args[0]);
    return xr_value_from_map(map);
}

// cdata(content)
static XrValue xml_cdata_fn(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) return xr_null();

    XmlKeys k;
    xml_keys_init(X, &k);

    XrMap *map = xr_map_new(xr_current_coro(X));
    xr_map_set(map, k.type,
               xr_string_value(xr_string_intern(X, "cdata", 5, 0)));
    xr_map_set(map, k.text, args[0]);
    return xr_value_from_map(map);
}

// ========== Module loading ===========

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module xml

XR_DEFINE_BUILTIN(xml_parse_fn, "parse", "(data: string, options?: Json): Json?", "Parse XML string")
XR_DEFINE_BUILTIN(xml_parse_detailed, "parseDetailed", "(data: string): Json?", "Parse XML with details")
XR_DEFINE_BUILTIN(xml_parse_file, "parseFile", "(path: string): Json?", "Parse XML file")
XR_DEFINE_BUILTIN(xml_stringify_fn, "stringify", "(node: Json, options?: Json): string", "Convert to XML string")
XR_DEFINE_BUILTIN(xml_write_file, "writeFile", "(path: string, node: Json): bool", "Write XML file")
XR_DEFINE_BUILTIN(xml_document_fn, "document", "(): Json", "Create XML document node")
XR_DEFINE_BUILTIN(xml_element_fn, "element", "(name: string, attrs?: Json): Json", "Create XML element node")
XR_DEFINE_BUILTIN(xml_text_fn, "text", "(content: string): Json", "Create XML text node")
XR_DEFINE_BUILTIN(xml_comment_fn, "comment", "(content: string): Json", "Create XML comment node")
XR_DEFINE_BUILTIN(xml_cdata_fn, "cdata", "(content: string): Json", "Create XML CDATA node")

XrModule* xr_load_module_xml(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "xml");
    if (!mod) return NULL;

    extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
    extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);

    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, mod, name_str, fn_val); \
        } while(0)

    EXPORT_CFUNC("parse", xml_parse_fn);
    EXPORT_CFUNC("parseDetailed", xml_parse_detailed);
    EXPORT_CFUNC("parseFile", xml_parse_file);
    EXPORT_CFUNC("stringify", xml_stringify_fn);
    EXPORT_CFUNC("writeFile", xml_write_file);
    EXPORT_CFUNC("document", xml_document_fn);
    EXPORT_CFUNC("element", xml_element_fn);
    EXPORT_CFUNC("text", xml_text_fn);
    EXPORT_CFUNC("comment", xml_comment_fn);
    EXPORT_CFUNC("cdata", xml_cdata_fn);

    #undef EXPORT_CFUNC

    mod->loaded = true;
    return mod;
}
