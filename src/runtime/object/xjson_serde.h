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
 *   Used by the JSON namespace (xjson_builtins.c) and the embedding API.
 *   RFC 8259 compliant. Enum values use their member name; values outside
 *   the JSON.Encodable domain are rejected.
 */

#ifndef XJSON_SERDE_H
#define XJSON_SERDE_H

#include "../value/xvalue.h"

typedef struct XrJsonDecodeSchema XrJsonDecodeSchema;

/* ========== Stringify Result (error-returning, no VM dependency) ========== */

typedef struct {
    XrValue result;       // serialized string, or xr_null() on error
    bool has_error;       // true when a non-serializable type was encountered
    char error_msg[128];  // human-readable description of the offending type
} XrJsonStringifyResult;

typedef struct {
    XrValue result;       // encoded Json value, or xr_null() on error
    bool has_error;       // true when a non-encodable value was encountered
    char error_msg[128];  // human-readable description of the offending type
} XrJsonEncodeResult;

typedef struct {
    XrValue result;  // parsed value; valid JSON null is represented by xr_null()
    bool has_error;  // distinguishes valid JSON null from malformed input
} XrJsonParseResult;

typedef struct XrJsonTypedParseError {
    char path[160];
    char expected[48];
    char actual[48];
} XrJsonTypedParseError;

struct XrClass;
struct XrCoroutine;

/* ========== Script-callable Functions ========== */

// Parse without throwing. Script-facing wrappers turn has_error into a panic.
XR_FUNC XrJsonParseResult xr_json_parse_core(XrVMRuntime *X, XrValue text);

// Core stringify: returns result + error info without throwing.
// Callers that need exception semantics should inspect has_error and throw.
XR_FUNC XrJsonStringifyResult xr_json_stringify_core(XrVMRuntime *X, XrValue val, int indent);

// Core encode: converts typed values into a Json value tree without a
// stringify/parse round-trip. Callers decide whether to throw on errors.
XR_FUNC XrJsonEncodeResult xr_json_encode_core(XrVMRuntime *X, XrValue val);

// isValid(str, strict?) → bool (zero-allocation validator)
XR_FUNC XrValue xr_json_fn_is_valid(XrVMRuntime *X, XrValue self, XrValue *args, int argc);

/* ========== C API ========== */

// Serialize XrValue to a malloc'd C-string (caller frees with xr_free)
XR_FUNC char *xr_json_stringify_to_cstr(XrVMRuntime *X, XrValue val, size_t *out_len);

// Parse JSON C-string to XrValue (returns xr_null() on error)
XR_FUNC XrValue xr_json_parse_from_cstr(XrVMRuntime *X, const char *json_str, size_t len);
XR_FUNC bool xr_json_parse_typed_object_from_cstr(XrVMRuntime *X, struct XrCoroutine *coro,
                                                  const char *json_str, size_t len,
                                                  struct XrClass *target_class,
                                                  bool ignore_unknown_fields, XrValue *out,
                                                  XrJsonTypedParseError *error);
XR_FUNC bool xr_json_parse_typed_value_from_cstr(XrVMRuntime *X, struct XrCoroutine *coro,
                                                 const char *json_str, size_t len,
                                                 const XrJsonDecodeSchema *schema,
                                                 bool ignore_unknown_fields, XrValue *out,
                                                 XrJsonTypedParseError *error);
XR_FUNC bool xr_json_parse_typed_object_with_rest_from_cstr(
    XrVMRuntime *X, struct XrCoroutine *coro, const char *json_str, size_t len,
    struct XrClass *target_class, struct XrClass *wrapper_class, bool ignore_nested_unknown_fields,
    XrValue *out, XrJsonTypedParseError *error);

#endif  // XJSON_SERDE_H
