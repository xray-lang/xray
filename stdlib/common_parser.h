/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * common_parser.h - Shared helpers for stdlib parser modules
 *
 * KEY CONCEPT:
 *   JSON / YAML / TOML / XML / CSV parsers all produce error Maps with the
 *   same schema { type, line?, row?, column?, message } and all extract
 *   config bool/int/char/fixed-string fields from an XrJson object. This
 *   header centralises both:
 *
 *   1. XR_STDLIB_MAX_DEPTH — the recommended default nesting cap shared
 *      by every parser (individual parsers may still override in their
 *      own config if they need a stricter limit).
 *
 *   2. xrs_err_keys_get() / xrs_error_map_push() — build a standard error
 *      Map once instead of open-coding four xr_string_intern() calls at
 *      every error site. Keys are cached per-isolate in
 *      XrStdlibCache::err_keys so the hash lookup runs at most once per
 *      isolate lifetime.
 *
 *   3. xrs_cfg_get_bool / _int / _char / _fixed_str — convenience
 *      readers for the XrJson -> struct config path.
 *
 * WHY THIS DESIGN:
 *   Per the stdlib_serialization analysis (docs/analysis), the error-map
 *   and config-extract boilerplate was duplicated across 5 modules for
 *   ~240 lines of drift-prone copy-paste. Concentrating it here keeps
 *   behaviour (interning, range checks, truncation) identical across the
 *   stdlib and makes future schema changes single-point.
 */

#ifndef XR_STDLIB_COMMON_PARSER_H
#define XR_STDLIB_COMMON_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../src/runtime/value/xvalue.h"
#include "../src/runtime/object/xstring.h"
#include "../src/runtime/object/xarray.h"
#include "../src/runtime/object/xmap.h"
#include "../src/runtime/object/xjson.h"
#include "../src/runtime/xisolate_internal.h"

#include "stdlib_cache.h"

// ========== Recommended default max nesting depth ==========

// 256 is deep enough for all realistic hand-authored configs and compact
// enough to catch pathological inputs (deeply-nested JSON, YAML, TOML
// dotted keys, XML elements, etc.). Individual parsers may still lower
// this value in their own config; raising it beyond XR_STDLIB_MAX_DEPTH
// is discouraged because the surrounding call stacks have not been
// audited for deeper recursion.
#define XR_STDLIB_MAX_DEPTH 256

// ========== Error-map key cache ==========

// Populate the per-isolate err_keys cache on first access and return it.
// Callers must not free or mutate the returned struct. The cache owns the
// XrValue handles for the isolate's lifetime.
static inline XrStdlibErrKeys* xrs_err_keys_get(XrayIsolate *X) {
    XrStdlibCache *c = xr_stdlib_cache_get(X);
    XrStdlibErrKeys *k = &c->err_keys;
    if (!k->ready) {
        k->type    = xr_string_value(xr_string_intern(X, "type",    4, 0));
        k->line    = xr_string_value(xr_string_intern(X, "line",    4, 0));
        k->row     = xr_string_value(xr_string_intern(X, "row",     3, 0));
        k->column  = xr_string_value(xr_string_intern(X, "column",  6, 0));
        k->message = xr_string_value(xr_string_intern(X, "message", 7, 0));
        k->ready = true;
    }
    return k;
}

// Build a standard error Map and push it onto `errors`.
//
// Semantics:
//   * `type_literal`   : short identifier (e.g. "UnterminatedQuote"). Must
//                        be a NUL-terminated C literal. Interned on first
//                        use but re-interned per call (cheap: hash hit).
//   * `line`, `column` : 1-based. Pass -1 to skip the respective field.
//   * `row`            : CSV-style row counter. Pass -1 to skip.
//   * `msg`            : human-readable detail. Copied via interning.
//
// The function never fails; on allocator OOM the underlying xr_map_new /
// xr_array_push already abort consistently with the rest of stdlib.
static inline void xrs_error_push(XrayIsolate *X,
                                  XrArray *errors,
                                  const char *type_literal,
                                  int line,
                                  int row,
                                  int column,
                                  const char *msg)
{
    XrStdlibErrKeys *keys = xrs_err_keys_get(X);
    XrMap *err = xr_map_new(xr_current_coro(X));

    XrValue v_type = xr_string_value(xr_string_intern(X, type_literal,
                                                     strlen(type_literal), 0));
    xr_map_set(err, keys->type, v_type);

    if (line >= 0) {
        xr_map_set(err, keys->line, xr_int(line));
    }
    if (row >= 0) {
        xr_map_set(err, keys->row, xr_int(row));
    }
    if (column >= 0) {
        xr_map_set(err, keys->column, xr_int(column));
    }

    if (msg) {
        XrValue v_msg = xr_string_value(xr_string_intern(X, msg,
                                                         strlen(msg), 0));
        xr_map_set(err, keys->message, v_msg);
    }

    xr_array_push(errors, xr_value_from_map(err));
}

// ========== Config extraction helpers ==========

// Read a boolean-typed config field by key. If absent or wrong type, the
// caller-supplied `*dst` is left untouched (preserving the default that
// config_init() already stored).
static inline void xrs_cfg_get_bool(XrayIsolate *X, XrJson *json,
                                    const char *key, bool *dst)
{
    if (!json || !dst) return;
    XrValue v = xr_json_get_by_key(X, json, key);
    if (XR_IS_BOOL(v)) *dst = XR_TO_BOOL(v);
}

// Read an int32-typed config field by key; silently ignores non-int
// values. Truncates negative sentinel values (e.g. -1) unchanged.
static inline void xrs_cfg_get_int(XrayIsolate *X, XrJson *json,
                                   const char *key, int *dst)
{
    if (!json || !dst) return;
    XrValue v = xr_json_get_by_key(X, json, key);
    if (XR_IS_INT(v)) *dst = (int)XR_TO_INT(v);
}

// Read a single-char-typed config field (the first byte of the string
// argument is taken). Empty strings leave *dst unchanged.
static inline void xrs_cfg_get_char(XrayIsolate *X, XrJson *json,
                                    const char *key, char *dst)
{
    if (!json || !dst) return;
    XrValue v = xr_json_get_by_key(X, json, key);
    if (XR_IS_STRING(v)) {
        XrString *s = XR_TO_STRING(v);
        if (s->length > 0) *dst = s->data[0];
    }
}

// Read a string-typed config field into an owned fixed-size buffer
// (NUL-terminated). The buffer size `dst_cap` must be at least 1. If the
// source is longer than `dst_cap - 1`, it is truncated. Returns the
// number of bytes copied (not counting the trailing NUL); 0 means the
// key was absent or not a string, in which case `dst` is untouched.
static inline size_t xrs_cfg_get_fixed_str(XrayIsolate *X, XrJson *json,
                                           const char *key,
                                           char *dst, size_t dst_cap)
{
    if (!json || !dst || dst_cap == 0) return 0;
    XrValue v = xr_json_get_by_key(X, json, key);
    if (!XR_IS_STRING(v)) return 0;
    XrString *s = XR_TO_STRING(v);
    size_t n = s->length < dst_cap - 1 ? s->length : dst_cap - 1;
    memcpy(dst, s->data, n);
    dst[n] = '\0';
    return n;
}

#endif // XR_STDLIB_COMMON_PARSER_H
