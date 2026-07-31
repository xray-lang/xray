/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_accessor_name.h - Encoding of computed-property accessors as method names.
 *
 * KEY CONCEPT:
 *   A computed property `x: int { fn() {...} fn(v) {...} }` has no slot. The
 *   parser stores its accessors as ordinary methods named "get:x" / "set:x",
 *   and every later stage recognises them by that name: the analyzer resolves
 *   `obj.x` to one of them, and lowering emits the call. At runtime an
 *   accessor is an ordinary closure method -- the name is the only marker.
 *   ':' cannot appear in an identifier, so an accessor name can never collide
 *   with a declared method.
 *
 *   The encoding lives here so those stages share one definition instead of
 *   each spelling the prefix out.
 */

#ifndef XR_ACCESSOR_NAME_H
#define XR_ACCESSOR_NAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define XR_ACCESSOR_GET_PREFIX "get:"
#define XR_ACCESSOR_SET_PREFIX "set:"
#define XR_ACCESSOR_PREFIX_LEN 4

/* Longest accessor name a fixed buffer must hold. Identifiers past this are
 * rejected by xr_accessor_name rather than silently truncated into a name that
 * would resolve to the wrong property. */
#define XR_ACCESSOR_NAME_MAX 256

/* Build "get:<prop>" / "set:<prop>". `prefix` is one of the two macros above.
 * Returns false when the result would not fit, leaving `buf` untouched. */
static inline bool xr_accessor_name(char *buf, size_t size, const char *prefix, const char *prop) {
    if (!buf || !prefix || !prop || size == 0)
        return false;
    size_t need = strlen(prefix) + strlen(prop) + 1;
    if (need > size)
        return false;
    snprintf(buf, size, "%s%s", prefix, prop);
    return true;
}

static inline bool xr_accessor_is_getter(const char *method_name) {
    return method_name && strncmp(method_name, XR_ACCESSOR_GET_PREFIX, XR_ACCESSOR_PREFIX_LEN) == 0;
}

static inline bool xr_accessor_is_setter(const char *method_name) {
    return method_name && strncmp(method_name, XR_ACCESSOR_SET_PREFIX, XR_ACCESSOR_PREFIX_LEN) == 0;
}

static inline bool xr_accessor_is_accessor(const char *method_name) {
    return xr_accessor_is_getter(method_name) || xr_accessor_is_setter(method_name);
}

/* The property an accessor method serves: "get:width" -> "width". Returns NULL
 * for a name that is not an accessor. */
static inline const char *xr_accessor_property_name(const char *method_name) {
    return xr_accessor_is_accessor(method_name) ? method_name + XR_ACCESSOR_PREFIX_LEN : NULL;
}

#endif  // XR_ACCESSOR_NAME_H
