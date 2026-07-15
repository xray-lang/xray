/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_json_type.h - Runtime-neutral Json.decode field type contract
 */

#ifndef XR_JSON_TYPE_H
#define XR_JSON_TYPE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum XrJsonValueKind {
    XR_JSON_VALUE_ANY = 0,
    XR_JSON_VALUE_NULL = 1,
    XR_JSON_VALUE_BOOL = 2,
    XR_JSON_VALUE_INT = 3,
    XR_JSON_VALUE_FLOAT = 4,
    XR_JSON_VALUE_STRING = 5,
    XR_JSON_VALUE_JSON = 6,
} XrJsonValueKind;

enum {
    XR_JSON_VALUE_KIND_MASK = 0x7fu,
    XR_JSON_VALUE_NULLABLE = 0x80u,
};

static inline uint8_t xr_json_value_kind_base(uint8_t encoded) {
    return encoded & XR_JSON_VALUE_KIND_MASK;
}

static inline bool xr_json_value_kind_is_nullable(uint8_t encoded) {
    return (encoded & XR_JSON_VALUE_NULLABLE) != 0;
}

#endif
