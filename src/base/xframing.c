/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xframing.c - Content-Length framing protocol implementation
 */

#include "xframing.h"
#include "xchecks.h"
#include "xstrcompat.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/*
 * Case-insensitive search for needle in haystack (bounded by hay_end).
 * Returns pointer to start of match, or NULL.
 */
static const char *find_header_field(const char *haystack, const char *hay_end, const char *needle,
                                     size_t needle_len) {
    for (const char *p = haystack; p + needle_len <= hay_end; p++) {
        if (xr_strncasecmp(p, needle, needle_len) == 0) {
            return p;
        }
    }
    return NULL;
}

XrFrameStatus xr_frame_parse(const char *buf, size_t buf_len, size_t *header_end,
                             int *content_length) {
    if (!buf || !header_end || !content_length)
        return XR_FRAME_ERROR;

    /* Find the header/body separator */
    const char *sep = NULL;
    for (size_t i = 0; i + 3 < buf_len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            sep = buf + i;
            break;
        }
    }
    if (!sep)
        return XR_FRAME_PARTIAL;

    size_t hend = (size_t) (sep - buf) + 4;

    /* Find Content-Length header within the header block */
    static const char CL[] = "Content-Length:";
    const char *cl = find_header_field(buf, sep, CL, sizeof(CL) - 1);
    if (!cl)
        return XR_FRAME_ERROR;

    /* Skip "Content-Length:" and whitespace */
    const char *val = cl + (sizeof(CL) - 1);
    while (val < sep && isspace((unsigned char) *val))
        val++;

    /* Parse the integer value */
    int n = 0;
    bool has_digit = false;
    while (val < sep && *val >= '0' && *val <= '9') {
        n = n * 10 + (*val - '0');
        has_digit = true;
        val++;
    }
    if (!has_digit || n < 0)
        return XR_FRAME_ERROR;

    /* Check if the full body has arrived */
    if (buf_len < hend + (size_t) n) {
        /* Header parsed OK but body incomplete — store partial result
         * so caller can avoid re-parsing the header on the next attempt. */
        *header_end = hend;
        *content_length = n;
        return XR_FRAME_PARTIAL;
    }

    *header_end = hend;
    *content_length = n;
    return XR_FRAME_OK;
}

int xr_frame_write_header(char *buf, size_t buf_size, size_t body_len) {
    if (!buf || buf_size == 0)
        return -1;

    int n = snprintf(buf, buf_size, "Content-Length: %zu\r\n\r\n", body_len);
    XR_DCHECK(n > 0, "xr_frame_write_header: snprintf failed");

    if (n < 0 || (size_t) n >= buf_size)
        return -1;
    return n;
}
