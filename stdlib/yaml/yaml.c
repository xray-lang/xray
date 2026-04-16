/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml.c - YAML module binding
 *
 * KEY CONCEPT:
 *   Module interface connecting xray with YAML parser/serializer.
 */

#include "yaml.h"
#include "yaml_parser.h"
#include "../../src/base/xmalloc.h"
#include <stdio.h>
#include <string.h>

// Emitter function
extern XrValue yaml_emit(XrayIsolate *isolate, XrValue value, int indent, int flow_level, int line_width);

// ========== Public API ==========

XrValue xr_yaml_parse(XrayIsolate *isolate, const char *data, size_t len) {
    YamlConfig config;
    yaml_config_init(&config);
    
    YamlParser parser;
    yaml_parser_init(&parser, isolate, data, len, &config);
    XrValue result = yaml_parser_parse(&parser);
    yaml_parser_cleanup(&parser);
    
    return result;
}

XrValue xr_yaml_parse_all(XrayIsolate *isolate, const char *data, size_t len) {
    YamlConfig config;
    yaml_config_init(&config);
    
    YamlParser parser;
    yaml_parser_init(&parser, isolate, data, len, &config);
    XrArray *docs = yaml_parser_parse_all(&parser);
    yaml_parser_cleanup(&parser);
    
    return xr_value_from_array(docs);
}

XrValue xr_yaml_stringify(XrayIsolate *isolate, XrValue value, int indent) {
    return yaml_emit(isolate, value, indent, -1, 80);
}

// ========== Module Functions ==========

// parse(str, config?)
static XrValue yaml_parse(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }
    XrString *str = XR_TO_STRING(args[0]);
    
    YamlConfig config;
    yaml_config_init(&config);
    
    if (argc >= 2 && xr_value_is_json(args[1])) {
        XrJson *json = xr_value_to_json(args[1]);
        yaml_config_from_json(X, &config, json);
    }
    
    YamlParser parser;
    yaml_parser_init(&parser, X, str->data, str->length, &config);
    XrValue result = yaml_parser_parse(&parser);
    yaml_parser_cleanup(&parser);
    
    return result;
}

// parseStrict(str)
static XrValue yaml_parse_strict(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        XrMap *result = xr_map_new(xr_current_coro(X));
        xr_map_set(result, 
            xr_string_value(xr_string_intern(X, "data", 4, 0)),
            xr_null());
        xr_map_set(result,
            xr_string_value(xr_string_intern(X, "errors", 6, 0)),
            xr_value_from_array(xr_array_new(xr_current_coro(X))));
        return xr_value_from_map(result);
    }
    XrString *str = XR_TO_STRING(args[0]);
    
    YamlConfig config;
    yaml_config_init(&config);
    config.safe = true;
    
    YamlParser parser;
    yaml_parser_init(&parser, X, str->data, str->length, &config);
    YamlResult parse_result = yaml_parser_parse_strict(&parser);
    yaml_parser_cleanup(&parser);
    
    XrMap *result = xr_map_new(xr_current_coro(X));
    xr_map_set(result,
        xr_string_value(xr_string_intern(X, "data", 4, 0)),
        parse_result.data);
    xr_map_set(result,
        xr_string_value(xr_string_intern(X, "errors", 6, 0)),
        xr_value_from_array(parse_result.errors));
    
    XrMap *meta = xr_map_new(xr_current_coro(X));
    xr_map_set(meta,
        xr_string_value(xr_string_intern(X, "lines", 5, 0)),
        xr_int(parse_result.meta.lines));
    xr_map_set(meta,
        xr_string_value(xr_string_intern(X, "documents", 9, 0)),
        xr_int(parse_result.meta.documents));
    xr_map_set(meta,
        xr_string_value(xr_string_intern(X, "anchors", 7, 0)),
        xr_int(parse_result.meta.anchors));
    xr_map_set(result,
        xr_string_value(xr_string_intern(X, "meta", 4, 0)),
        xr_value_from_map(meta));
    
    return xr_value_from_map(result);
}

// parseAll(str)
static XrValue yaml_parse_all(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_value_from_array(xr_array_new(xr_current_coro(X)));
    }
    XrString *str = XR_TO_STRING(args[0]);
    return xr_yaml_parse_all(X, str->data, str->length);
}

// stringify(value, config?)
static XrValue yaml_stringify(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1) {
        return xr_string_value(xr_string_intern(X, "", 0, 0));
    }
    
    int indent = 2;
    int flow_level = -1;
    int line_width = 80;
    
    if (argc >= 2 && xr_value_is_json(args[1])) {
        XrJson *json = xr_value_to_json(args[1]);
        
        XrValue val = xr_json_get_by_key(X, json, "indent");
        if (XR_IS_INT(val)) {
            indent = (int)XR_TO_INT(val);
        }
        
        val = xr_json_get_by_key(X, json, "flowLevel");
        if (XR_IS_INT(val)) {
            flow_level = (int)XR_TO_INT(val);
        }
        
        val = xr_json_get_by_key(X, json, "lineWidth");
        if (XR_IS_INT(val)) {
            line_width = (int)XR_TO_INT(val);
        }
    } else if (argc >= 2 && XR_IS_INT(args[1])) {
        indent = (int)XR_TO_INT(args[1]);
    }
    
    return yaml_emit(X, args[0], indent, flow_level, line_width);
}

// parseFile(path)
static XrValue yaml_parse_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 1 || !XR_IS_STRING(args[0])) {
        return xr_null();
    }
    XrString *path = XR_TO_STRING(args[0]);
    
    FILE *f = fopen(path->data, "rb");
    if (!f) return xr_null();
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) { fclose(f); return xr_null(); }
    fseek(f, 0, SEEK_SET);
    
    char *content = (char*)xr_malloc(size + 1);
    if (!content) { fclose(f); return xr_null(); }
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);
    
    XrValue result = xr_yaml_parse(X, content, read_size);
    xr_free(content);
    return result;
}

// writeFile(path, value)
static XrValue yaml_write_file(XrayIsolate *X, XrValue *args, int argc) {
    if (argc < 2 || !XR_IS_STRING(args[0])) {
        return xr_bool(false);
    }
    XrString *path = XR_TO_STRING(args[0]);
    XrValue yaml_str = xr_yaml_stringify(X, args[1], 2);
    
    if (!XR_IS_STRING(yaml_str)) return xr_bool(false);
    XrString *str = XR_TO_STRING(yaml_str);
    
    FILE *f = fopen(path->data, "wb");
    if (!f) return xr_bool(false);
    fwrite(str->data, 1, str->length, f);
    fclose(f);
    return xr_bool(true);
}

// ========== Module Loading ===========

// ========== Type Declarations (parsed by gen_stdlib_types.py) ==========

#include "../../src/module/xbuiltin_decl.h"

// @module yaml

XR_DEFINE_BUILTIN(yaml_parse, "parse", "(data: string): Json?", "Parse YAML string")
XR_DEFINE_BUILTIN(yaml_parse_strict, "parseStrict", "(data: string): Json?", "Parse YAML strictly")
XR_DEFINE_BUILTIN(yaml_parse_all, "parseAll", "(data: string): Array<Json>", "Parse all YAML documents")
XR_DEFINE_BUILTIN(yaml_stringify, "stringify", "(value: Json): string", "Convert to YAML string")
XR_DEFINE_BUILTIN(yaml_parse_file, "parseFile", "(path: string): Json?", "Parse YAML file")
XR_DEFINE_BUILTIN(yaml_write_file, "writeFile", "(path: string, value: Json): bool", "Write YAML file")

XrModule* xr_load_module_yaml(XrayIsolate *isolate) {
    XrModule *mod = xr_module_create_native(isolate, "yaml");
    if (!mod) return NULL;
    
    extern XrCFunction* xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name);
    extern XrValue xr_value_from_cfunction(XrCFunction *cfunc);
    
    #define EXPORT_CFUNC(name_str, func_ptr) \
        do { \
            XrCFunction *cfunc = xr_vm_cfunction_new(isolate, func_ptr, name_str); \
            XrValue fn_val = xr_value_from_cfunction(cfunc); \
            xr_module_add_export(isolate, mod, name_str, fn_val); \
        } while(0)
    
    EXPORT_CFUNC("parse", yaml_parse);
    EXPORT_CFUNC("parseStrict", yaml_parse_strict);
    EXPORT_CFUNC("parseAll", yaml_parse_all);
    EXPORT_CFUNC("stringify", yaml_stringify);
    EXPORT_CFUNC("parseFile", yaml_parse_file);
    EXPORT_CFUNC("writeFile", yaml_write_file);
    
    #undef EXPORT_CFUNC
    
    mod->loaded = true;
    return mod;
}
