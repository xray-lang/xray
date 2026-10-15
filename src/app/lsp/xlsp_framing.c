/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_framing.c - Exact header fields and bounded decimal frame lengths
 *
 * KEY CONCEPT:
 *   Metadata is ASCII and CRLF delimited. Length fields cannot be discovered
 *   inside another field or accept a second value, suffix or numeric overflow.
 */
#include "xlsp_framing.h"
#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
_Static_assert(XLSP_FRAME_MAX_BODY <= INT_MAX, "Frame lengths fit the transport's checked integer field");
static unsigned char frame_lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c; }
static bool frame_equal(const char *a, size_t length, const char *b) {
    if (length != strlen(b)) return false;
    for (size_t i = 0; i < length; ++i) if (frame_lower((unsigned char)a[i]) != (unsigned char)b[i]) return false;
    return true;
}
static bool frame_token(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        (c && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}
static void frame_space(const char **at, const char *end) { while (*at < end && (**at == ' ' || **at == '\t')) ++*at; }
static bool frame_number(const char *at, const char *end, unsigned *out) {
    frame_space(&at, end); unsigned value = 0; bool digit = false;
    while (at < end && *at >= '0' && *at <= '9') {
        unsigned next = (unsigned)(*at++ - '0');
        if (value > (XLSP_FRAME_MAX_BODY - next) / 10) return false;
        value = value * 10 + next; digit = true;
    }
    frame_space(&at, end); if (!digit || at != end) return false;
    *out = value; return true;
}
static bool frame_charset(const char *value, size_t length) {
    static const char expected[] = "utf-8"; size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        if (value[i] == '\\' && ++i == length) return false;
        if (used == sizeof(expected) - 1 || frame_lower((unsigned char)value[i]) != (unsigned char)expected[used++]) return false;
    }
    return used == sizeof(expected) - 1;
}
static bool frame_content_type(const char *at, const char *end) {
    frame_space(&at, end); const char *start = at;
    while (at < end && frame_token((unsigned char)*at)) ++at;
    if (at == start || at == end || *at++ != '/') return false;
    start = at; while (at < end && frame_token((unsigned char)*at)) ++at;
    if (at == start) return false;
    bool charset = false; frame_space(&at, end);
    while (at < end) {
        if (*at++ != ';') return false;
        frame_space(&at, end); const char *name = at;
        while (at < end && frame_token((unsigned char)*at)) ++at;
        size_t name_length = (size_t)(at - name); if (!name_length) return false;
        frame_space(&at, end); if (at == end || *at++ != '=') return false;
        frame_space(&at, end); const char *value = at; size_t length = 0;
        if (at < end && *at == '"') {
            value = ++at;
            while (at < end && *at != '"') {
                if (*at == '\\' && ++at == end) return false;
                ++at;
            }
            if (at == end) return false;
            length = (size_t)(at - value); ++at;
        } else {
            while (at < end && frame_token((unsigned char)*at)) ++at;
            length = (size_t)(at - value); if (!length) return false;
        }
        if (frame_equal(name, name_length, "charset")) {
            if (charset || !frame_charset(value, length)) return false;
            charset = true;
        }
        frame_space(&at, end);
    }
    return true;
}
static bool frame_fields(const char *buffer, size_t header, unsigned *length) {
    const char *at = buffer, *end = buffer + header - 2; bool found = false, content_type = false;
    while (at < end) {
        const char *name = at;
        while (at < end && frame_token((unsigned char)*at)) ++at;
        size_t name_length = (size_t)(at - name); if (!name_length || at == end || *at++ != ':') return false;
        const char *value = at; while (at < end && *at != '\r') ++at;
        if (at == end || end - at < 2 || at[1] != '\n') return false;
        if (frame_equal(name, name_length, "content-length")) {
            if (found || !frame_number(value, at, length)) return false;
            found = true;
        } else if (frame_equal(name, name_length, "content-type")) {
            if (content_type || !frame_content_type(value, at)) return false;
            content_type = true;
        }
        at += 2;
    }
    return found && at == end;
}
XR_FUNC XlspFrameStatus xlsp_frame_read(const char *buffer, size_t count, size_t *header_end, int *content_length) {
    if (!buffer || !header_end || !content_length) return XLSP_FRAME_ERROR;
    size_t header = 0, limit = count < XLSP_FRAME_MAX_HEADER ? count : XLSP_FRAME_MAX_HEADER;
    for (size_t i = 0; i < limit; ++i) {
        unsigned char c = (unsigned char)buffer[i];
        if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c >= 127) return XLSP_FRAME_ERROR;
        if (c == '\n' && (!i || buffer[i - 1] != '\r')) return XLSP_FRAME_ERROR;
        if (c == '\r') {
            if (i + 1 == limit) break;
            if (buffer[i + 1] != '\n') return XLSP_FRAME_ERROR;
            if (limit - i >= 4 && buffer[i + 2] == '\r' && buffer[i + 3] == '\n') { header = i + 4; break; }
        }
    }
    if (!header) return count >= XLSP_FRAME_MAX_HEADER ? XLSP_FRAME_ERROR : XLSP_FRAME_PARTIAL;
    unsigned length = 0; if (!frame_fields(buffer, header, &length)) return XLSP_FRAME_ERROR;
    *header_end = header; *content_length = (int)length;
    return count - header >= length ? XLSP_FRAME_OK : XLSP_FRAME_PARTIAL;
}
XR_FUNC int xlsp_frame_header(char *buffer, size_t capacity, size_t length) {
    if (!buffer || !capacity || length > XLSP_FRAME_MAX_BODY) return -1;
    int count = snprintf(buffer, capacity, "Content-Length: %zu\r\n\r\n", length);
    return count < 0 || (size_t)count >= capacity ? -1 : count;
}
