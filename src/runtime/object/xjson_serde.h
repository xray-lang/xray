/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xjson_serde.h - JSON serialization/deserialization engine
 *
 * KEY CONCEPT:
 *   Core JSON serialize/deserialize between XrValue and JSON strings.
 *   Used by the Json builtin class (xjson_builtins.c) and the embedding
 *   API. RFC 8259 compliant. Handles Enum (member name), DateTime (ISO
 *   8601), and throws on non-serializable types (function, class, etc.).
 */

#ifndef XJSON_SERDE_H
#define XJSON_SERDE_H

#include "../value/xvalue.h"

/* ========== Script-callable Functions ========== */

// parse(str) → XrValue
XR_FUNC XrValue xr_json_fn_parse(XrayIsolate *X, XrValue *args, int argc);

// stringify(value, indent?) → string; throws on non-serializable types
XR_FUNC XrValue xr_json_fn_stringify(XrayIsolate *X, XrValue *args, int argc);

// isValid(str, strict?) → bool (zero-allocation validator)
XR_FUNC XrValue xr_json_fn_is_valid(XrayIsolate *X, XrValue *args, int argc);

// tryParse(str) → Json {value, error}
XR_FUNC XrValue xr_json_fn_try_parse(XrayIsolate *X, XrValue *args, int argc);

/* ========== C API ========== */

// Serialize XrValue to a malloc'd C-string (caller frees with xr_free)
XR_FUNC char *xr_json_stringify_to_cstr(XrayIsolate *X, XrValue val, size_t *out_len);

// Parse JSON C-string to XrValue (returns xr_null() on error)
XR_FUNC XrValue xr_json_parse_from_cstr(XrayIsolate *X, const char *json_str, size_t len);

#endif  // XJSON_SERDE_H
