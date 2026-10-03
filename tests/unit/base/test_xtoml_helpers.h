/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xtoml_helpers.h - Typed query adapters for independent syntax fixtures
 */
#include "base/xtoml.h"
#include <stdio.h>
#include <stdlib.h>
static inline XrTomlValue *test_toml_parse(const char *data, size_t length) {
    XrOsIoPolicy policy = xr_os_io_system_policy();
    const XrTomlParseLimits limits = {16u*1024u*1024u,128};
    XrTomlValue *output = NULL;
    XrTomlParseStatus status = xtoml_parse_owned(&policy,data,length,&limits,&output);
    if (status != XR_TOML_PARSE_OK && status != XR_TOML_PARSE_INVALID && status != XR_TOML_PARSE_LIMIT) exit(1);
    return output;
}
static inline XrTomlValue *test_toml_get(XrTomlValue *table, const char *key) {
    if (!table || table->type != XR_TOML_TABLE) return NULL;
    XrTomlValue *output = NULL;
    if (xtoml_owned_get(table,key,&output) != XR_TOML_PARSE_OK) exit(1);
    return output;
}
static inline const char *test_toml_get_string(XrTomlValue *table, const char *key) {
    XrTomlValue *v = test_toml_get(table,key); return v && v->type == XR_TOML_STRING ? v->as.string : NULL;
}
static inline int64_t test_toml_get_int_or(XrTomlValue *table, const char *key, int64_t fallback) {
    XrTomlValue *v = test_toml_get(table,key); return v && v->type == XR_TOML_INTEGER ? v->as.integer : fallback;
}
static inline int64_t test_toml_get_int(XrTomlValue *table, const char *key) { return test_toml_get_int_or(table,key,0); }
static inline double test_toml_get_float(XrTomlValue *table, const char *key) {
    XrTomlValue *v = test_toml_get(table,key); return v && v->type == XR_TOML_FLOAT ? v->as.number : 0.0;
}
static inline XrTomlValue *test_toml_get_array(XrTomlValue *table, const char *key) {
    XrTomlValue *v = test_toml_get(table,key); return v && v->type == XR_TOML_ARRAY ? v : NULL;
}
static inline XrTomlValue *test_toml_get_table(XrTomlValue *table, const char *key) {
    XrTomlValue *v = test_toml_get(table,key); return v && v->type == XR_TOML_TABLE ? v : NULL;
}
