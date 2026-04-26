/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtoml.h - Pure C TOML v1.0.0 parser (no runtime dependency)
 *
 * KEY CONCEPT:
 *   Builds a DOM tree of XrTomlValue nodes from TOML input. Lives at
 *   layer L0 (src/base/) so it can be used by LSP config, project
 *   loader, and the stdlib bridge without pulling in the VM runtime.
 *
 *   Datetime values are stored as ISO 8601 strings (no runtime
 *   DateTime object at this layer).
 */

#ifndef XTOML_H
#define XTOML_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xdefs.h"

/* ========== TOML Value Types ========== */

typedef enum {
    XR_TOML_STRING,
    XR_TOML_INTEGER,
    XR_TOML_FLOAT,
    XR_TOML_BOOL,
    XR_TOML_DATETIME, /* stored as ISO 8601 string */
    XR_TOML_ARRAY,
    XR_TOML_TABLE
} XrTomlType;

typedef struct XrTomlValue XrTomlValue;

typedef struct XrTomlMember {
    char *key;
    XrTomlValue *value;
} XrTomlMember;

struct XrTomlValue {
    XrTomlType type;
    union {
        char *string; /* XR_TOML_STRING / XR_TOML_DATETIME */
        int64_t integer;
        double number;
        bool boolean;
        struct {
            XrTomlValue **items;
            int count;
            int capacity;
        } array;
        struct {
            XrTomlMember *members;
            int count;
            int capacity;
        } table;
    } as;
};

/* ========== Parse / Free ========== */

/* Parse TOML text into a DOM tree. Returns root table, or NULL on
 * fatal error. The returned tree must be freed with xtoml_free(). */
XR_FUNC XrTomlValue *xtoml_parse(const char *data, size_t len);

/* Free a DOM tree (recursive). */
XR_FUNC void xtoml_free(XrTomlValue *v);

/* ========== Table Accessors ========== */

XR_FUNC XrTomlValue *xtoml_get(XrTomlValue *table, const char *key);
XR_FUNC const char *xtoml_get_string(XrTomlValue *table, const char *key);
XR_FUNC int64_t xtoml_get_int(XrTomlValue *table, const char *key);
XR_FUNC int64_t xtoml_get_int_or(XrTomlValue *table, const char *key, int64_t default_val);
XR_FUNC double xtoml_get_float(XrTomlValue *table, const char *key);
XR_FUNC bool xtoml_get_bool(XrTomlValue *table, const char *key);
XR_FUNC bool xtoml_get_bool_or(XrTomlValue *table, const char *key, bool default_val);
XR_FUNC XrTomlValue *xtoml_get_table(XrTomlValue *table, const char *key);
XR_FUNC XrTomlValue *xtoml_get_array(XrTomlValue *table, const char *key);

/* ========== Array Accessors ========== */

XR_FUNC int xtoml_array_len(XrTomlValue *arr);
XR_FUNC XrTomlValue *xtoml_array_get(XrTomlValue *arr, int index);

/* ========== Table Count ========== */

XR_FUNC int xtoml_table_count(XrTomlValue *table);

/* ========== Type Checks ========== */

XR_FUNC bool xtoml_is_string(XrTomlValue *v);
XR_FUNC bool xtoml_is_integer(XrTomlValue *v);
XR_FUNC bool xtoml_is_float(XrTomlValue *v);
XR_FUNC bool xtoml_is_bool(XrTomlValue *v);
XR_FUNC bool xtoml_is_datetime(XrTomlValue *v);
XR_FUNC bool xtoml_is_array(XrTomlValue *v);
XR_FUNC bool xtoml_is_table(XrTomlValue *v);

#endif  // XTOML_H
