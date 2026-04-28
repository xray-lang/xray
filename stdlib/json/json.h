/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * json.h - JSON parsing and serialization module
 *
 * KEY CONCEPT:
 *   Provides bidirectional JSON conversion between JSON strings and xray values.
 *   Supports all standard JSON types with proper xray type mapping.
 *
 * TYPE MAPPING:
 *   JSON -> xray:
 *     null    -> null
 *     boolean -> bool
 *     number  -> int/float
 *     string  -> string
 *     array   -> Array
 *     object  -> Json (access fields via obj.name)
 *
 *   xray -> JSON:
 *     null/bool/int/float/string -> corresponding JSON type
 *     Array                      -> JSON array
 *     Json/Map/Instance          -> JSON object
 */

#ifndef XR_STDLIB_JSON_H
#define XR_STDLIB_JSON_H

#include "../../src/base/xdefs.h"
#include "../../src/runtime/value/xvalue.h"

struct XrModule;

/*
 * Module exports:
 *   - parse(str)               Parse JSON string to xray value
 *   - stringify(value)         Serialize xray value to JSON string
 *   - stringify(value, indent) Pretty print with indentation (1-8)
 *   - isValid(str)             Check if string is valid JSON
 *   - typeOf(value)            Get JSON type name
 *   - tryParse(str)            Safe parse, returns {value, error}
 *   - keys(obj)                Get all keys of object
 *   - values(obj)              Get all values of object
 */
XR_FUNC XrModule *xr_load_module_json(XrayIsolate *isolate);

// Public API: serialize XrValue to JSON C-string.
// The returned buffer is allocated via xr_malloc; caller must release
// with xr_free (NOT the libc free).
XR_FUNC char *xr_json_stringify_to_cstr(XrayIsolate *X, XrValue val, size_t *out_len);

// Public API: parse JSON C-string to XrValue (returns xr_null() on error)
XR_FUNC XrValue xr_json_parse_from_cstr(XrayIsolate *X, const char *json_str, size_t len);

#endif  // XR_STDLIB_JSON_H
