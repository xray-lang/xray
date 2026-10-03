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
#include "xio_policy.h"

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

typedef enum {
    XR_TOML_TABLE_IMPLICIT,
    XR_TOML_TABLE_DOTTED,
    XR_TOML_TABLE_HEADER,
    XR_TOML_TABLE_INLINE
} XrTomlTableOrigin;

typedef struct XrTomlMember {
    char *key;
    size_t key_length;
    XrTomlValue *value;
} XrTomlMember;

struct XrTomlValue {
    XrTomlType type;
    uint32_t depth; /* Root is zero; every table member and array element adds one. */
    size_t string_length; /* String/datetime bytes, excluding the terminator. */
    union {
        char *string; /* XR_TOML_STRING / XR_TOML_DATETIME */
        int64_t integer;
        double number;
        bool boolean;
        struct {
            XrTomlValue **items;
            int count;
            int capacity;
            bool table_sequence; /* Created by [[headers]], never a value array. */
        } array;
        struct {
            XrTomlMember *members;
            int count;
            int capacity;
            XrTomlTableOrigin origin;
        } table;
    } as;
};

/* ========== Parse / Free ========== */

typedef struct XrTomlParseLimits {
    size_t input_bytes;
    uint32_t depth; /* Effective maximum is 128 for recursive destruction. */
} XrTomlParseLimits;

typedef enum XrTomlParseStatus {
    XR_TOML_PARSE_OK,
    XR_TOML_PARSE_INVALID,
    XR_TOML_PARSE_LIMIT,
    XR_TOML_PARSE_OUT_OF_MEMORY,
    XR_TOML_PARSE_BUDGET,
    XR_TOML_PARSE_IO,
    XR_TOML_PARSE_BAD_ARGUMENT
} XrTomlParseStatus;

/* Policy and limits are mandatory. All DOM blocks and temporary buffers use the
 * same policy; their private headers retain the allocation policy for release.
 * Compile-ledger blocks keep that ledger alive through the final free. Other
 * policy contexts must outlive their blocks. Work and cumulative allocations
 * are never refunded. Failure preserves output and frees the partial DOM. */
XR_FUNC XrTomlParseStatus xtoml_parse_owned(const XrOsIoPolicy *policy,
    const char *data, size_t len, const XrTomlParseLimits *limits, XrTomlValue **output);
XR_FUNC void xtoml_owned_free(XrTomlValue *value);
/* Only accepts an owned DOM node; compares its actual allocation policy. */
XR_FUNC bool xtoml_owned_uses_policy(const XrTomlValue *value, const XrOsIoPolicy *policy);

/* Key scanning and candidate comparisons use the table's allocation policy.
 * A missing key returns OK with NULL. Failure preserves output. */
XR_FUNC XrTomlParseStatus xtoml_owned_get(XrTomlValue *table,
    const char *key, XrTomlValue **output);
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
