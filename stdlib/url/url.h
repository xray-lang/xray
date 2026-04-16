/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * url.h - URL standard library
 *
 * KEY CONCEPT:
 *   RFC 3986 compliant URL parsing, encoding, and query parameter handling.
 *   All functions return xray values directly (Json, Map, string).
 *
 * EXPORTED FUNCTIONS:
 *   - parse(url)              Parse URL → Json {protocol, hostname, port, ...}
 *   - format(obj)             Build URL string from Json components
 *   - parseQuery(qs)          Parse query string → Json
 *   - buildQuery(obj)         Build query string from Json
 *   - encode(str)             RFC 3986 percent-encode (unreserved chars preserved)
 *   - decode(str)             RFC 3986 percent-decode
 *   - encodeForm(str)         application/x-www-form-urlencoded encode
 *   - decodeForm(str)         application/x-www-form-urlencoded decode
 *   - resolve(base, relative) Resolve relative URL against base
 *   - join(parts...)          Join URL path segments
 */

#ifndef XR_STDLIB_URL_H
#define XR_STDLIB_URL_H

#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"

// ========== URL Encoding (C-level API) ==========

// RFC 3986 percent-encode: preserves unreserved chars (A-Z a-z 0-9 - _ . ~)
int xr_url_encode(const char *str, size_t len, char *buf, size_t buf_size);

// RFC 3986 percent-decode: does NOT treat '+' as space
int xr_url_decode(const char *str, size_t len, char *buf, size_t buf_size);

// Form encode: like RFC 3986 but space → '+'
int xr_url_encode_form(const char *str, size_t len, char *buf, size_t buf_size);

// Form decode: like RFC 3986 but '+' → space
int xr_url_decode_form(const char *str, size_t len, char *buf, size_t buf_size);

// ========== Module Loading ==========

XrModule* xr_load_module_url(XrayIsolate *isolate);

#endif
