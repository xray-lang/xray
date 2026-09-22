/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_float_fmt.h - Single source of truth for float -> string formatting
 *
 * Shared verbatim by the VM and AOT runtimes so that print, toString, and
 * string concatenation render floats byte-identically across backends. The
 * format is 15 significant digits (%.15g) with a trailing ".0" appended when
 * the result has no decimal point or exponent (so integral floats read as
 * "2.0", not "2"). Keeping one implementation is what prevents the two
 * backends from drifting (e.g. %.14g vs %.15g, or a missing ".0").
 *
 * This is the formatter for human-readable output. JSON serialization uses a
 * separate shortest-round-trip formatter (%.15g then %.17g) and must not be
 * routed through here.
 */

#ifndef XR_FLOAT_FMT_H
#define XR_FLOAT_FMT_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Format `value` into `buf` (capacity `bufsz`). Returns the required length
 * excluding the NUL, even for a zero-capacity query or truncated output. */
static inline int xr_format_float(char *buf, size_t bufsz, double value) {
    char rendered[64];
    int len = snprintf(rendered, sizeof(rendered), "%.15g", value);
    if (len < 0 || (size_t)len >= sizeof(rendered)) {
        if (buf && bufsz) buf[0] = '\0';
        return -1;
    }
    if (!strchr(rendered, '.') && !strchr(rendered, 'e') && !strchr(rendered, 'E') &&
        (size_t)len + 2u < sizeof(rendered)) {
        rendered[len] = '.';
        rendered[len + 1] = '0';
        rendered[len + 2] = '\0';
        len += 2;
    }
    if (buf && bufsz) {
        size_t copied = (size_t)len < bufsz - 1u ? (size_t)len : bufsz - 1u;
        memcpy(buf, rendered, copied);
        buf[copied] = '\0';
    }
    return len;
}

#endif  // XR_FLOAT_FMT_H
