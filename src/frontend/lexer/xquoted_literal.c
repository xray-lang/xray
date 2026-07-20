/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xquoted_literal.c - Shared quoted-literal payload extraction
 */

#include "xquoted_literal.h"

#include <string.h>

#include "../../base/xmalloc.h"
#include "../../base/xutf8.h"

static int hex_value(uint8_t c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool xr_escaped_bytes_decode(const uint8_t *src, size_t length, uint8_t *dst, size_t *out_length,
                             const char **error) {
    size_t out = 0;
    for (size_t i = 0; i < length; i++) {
        if (src[i] != '\\' || i + 1 >= length) {
            dst[out++] = src[i];
            continue;
        }
        uint8_t escaped = src[++i];
        switch (escaped) {
            case 'n':
                dst[out++] = '\n';
                break;
            case 'r':
                dst[out++] = '\r';
                break;
            case 't':
                dst[out++] = '\t';
                break;
            case '\\':
                dst[out++] = '\\';
                break;
            case '"':
                dst[out++] = '"';
                break;
            case '\'':
                dst[out++] = '\'';
                break;
            case 'b':
                dst[out++] = '\b';
                break;
            case 'f':
                dst[out++] = '\f';
                break;
            case '0':
                dst[out++] = '\0';
                break;
            case '$':
                dst[out++] = '$';
                break;
            case '`':
                dst[out++] = '`';
                break;
            case 'x': {
                if (i + 2 >= length || hex_value(src[i + 1]) < 0 || hex_value(src[i + 2]) < 0) {
                    if (error)
                        *error = "byte escape must use exactly two hexadecimal digits";
                    return false;
                }
                dst[out++] = (uint8_t) ((hex_value(src[i + 1]) << 4) | hex_value(src[i + 2]));
                i += 2;
                break;
            }
            case 'u': {
                /* Spec EscapeSeq: 'u' HexDigit{4} | 'u{' HexDigit{1,6} '}'.
                 * Decoded output is always shorter than its source spelling
                 * (\uXXXX = 6 chars -> <=3 bytes; \u{...} >= 5 chars covers
                 * every 1..4-byte encoding), so dst capacity holds. */
                uint32_t cp = 0;
                if (i + 1 < length && src[i + 1] == '{') {
                    size_t p = i + 2;
                    int digits = 0;
                    while (p < length && src[p] != '}') {
                        int h = hex_value(src[p]);
                        if (h < 0) {
                            if (error)
                                *error = "invalid hex digit in unicode escape";
                            return false;
                        }
                        if (digits >= 6) {
                            if (error)
                                *error = "unicode escape must contain at most 6 hex digits";
                            return false;
                        }
                        cp = (cp << 4) | (uint32_t) h;
                        digits++;
                        p++;
                    }
                    if (digits == 0) {
                        if (error)
                            *error = "unicode escape requires at least one hex digit";
                        return false;
                    }
                    if (p >= length || src[p] != '}') {
                        if (error)
                            *error = "unterminated unicode escape";
                        return false;
                    }
                    i = p;
                } else {
                    if (i + 4 >= length || hex_value(src[i + 1]) < 0 || hex_value(src[i + 2]) < 0 ||
                        hex_value(src[i + 3]) < 0 || hex_value(src[i + 4]) < 0) {
                        if (error)
                            *error = "unicode escape must use exactly four hexadecimal digits "
                                     "(\\uXXXX) or \\u{...}";
                        return false;
                    }
                    cp = (uint32_t) ((hex_value(src[i + 1]) << 12) | (hex_value(src[i + 2]) << 8) |
                                     (hex_value(src[i + 3]) << 4) | hex_value(src[i + 4]));
                    i += 4;
                }
                int encoded = xr_unicode_is_scalar(cp) ? xr_utf8_encode(cp, (char *) dst + out) : 0;
                if (encoded <= 0) {
                    if (error)
                        *error = "unicode escape must be a valid Unicode scalar value";
                    return false;
                }
                out += (size_t) encoded;
                break;
            }
            default:
                dst[out++] = '\\';
                dst[out++] = escaped;
                break;
        }
    }
    *out_length = out;
    return true;
}

static bool inline_source_range(const Token *token, const uint8_t **start, const uint8_t **end,
                                const char **error) {
    const uint8_t *token_start = (const uint8_t *) token->start;
    const uint8_t *token_end = token_start + token->length;
    const uint8_t *opener = token_start + token->prefix_length;
    if (token->quote_count == 2) {
        *start = token_end;
        *end = token_end;
        return true;
    }
    if (token->quote_count != 1 || opener >= token_end || *opener != '"' || token_end[-1] != '"') {
        if (error)
            *error = "invalid inline quoted-literal boundary";
        return false;
    }
    *start = opener + 1;
    *end = token_end - 1;
    return true;
}

static bool block_source_range(const Token *token, const uint8_t **start, const uint8_t **end,
                               const uint8_t **margin, size_t *margin_length, const char **error) {
    const uint8_t *token_start = (const uint8_t *) token->start;
    const uint8_t *token_end = token_start + token->length;
    const uint8_t *body = token_start + token->prefix_length + token->quote_count;
    if (body >= token_end) {
        if (error)
            *error = "invalid block quoted-literal boundary";
        return false;
    }
    if (*body == '\r' && body + 1 < token_end && body[1] == '\n')
        body += 2;
    else if (*body == '\n')
        body++;
    else {
        if (error)
            *error = "block opening delimiter must be followed by a newline";
        return false;
    }
    const uint8_t *close_quotes = token_end - token->quote_count;
    const uint8_t *close_line = close_quotes;
    while (close_line > body && close_line[-1] != '\n')
        close_line--;
    for (const uint8_t *p = close_line; p < close_quotes; p++) {
        if (*p != ' ' && *p != '\t') {
            if (error)
                *error = "block closing delimiter indentation is invalid";
            return false;
        }
    }
    const uint8_t *content_end = close_line;
    if (content_end > body) {
        if (content_end[-1] != '\n') {
            if (error)
                *error = "block closing delimiter must begin on its own line";
            return false;
        }
        content_end--;
        if (content_end > body && content_end[-1] == '\r')
            content_end--;
    }
    *start = body;
    *end = content_end;
    *margin = close_line;
    *margin_length = (size_t) (close_quotes - close_line);
    return true;
}

static bool normalize_block(const uint8_t *start, const uint8_t *end, const uint8_t *margin,
                            size_t margin_length, uint8_t *out, size_t *out_length,
                            const char **error) {
    const uint8_t *p = start;
    size_t dst = 0;
    bool at_line_start = true;
    while (p < end) {
        if (at_line_start) {
            const uint8_t *line_end = p;
            while (line_end < end && *line_end != '\n' && *line_end != '\r')
                line_end++;
            if (line_end > p) {
                if ((size_t) (line_end - p) < margin_length ||
                    memcmp(p, margin, margin_length) != 0) {
                    if (error)
                        *error = "every non-empty block line must begin with the closing margin";
                    return false;
                }
                p += margin_length;
            }
            at_line_start = false;
        }
        if (p >= end)
            break;
        if (*p == '\n') {
            out[dst++] = '\n';
            p++;
            at_line_start = true;
        } else if (*p == '\r') {
            if (p + 1 >= end || p[1] != '\n') {
                if (error)
                    *error = "bare carriage return is not allowed in a block literal";
                return false;
            }
            out[dst++] = '\n';
            p += 2;
            at_line_start = true;
        } else {
            out[dst++] = *p++;
        }
    }
    *out_length = dst;
    return true;
}

bool xr_quoted_payload_decode(const Token *token, bool decode_escapes, XrQuotedPayload *out,
                              const char **error) {
    if (!token || !out || token->quoted_kind == XR_QUOTED_NONE) {
        if (error)
            *error = "token is not a quoted literal";
        return false;
    }
    out->bytes = NULL;
    out->length = 0;
    const uint8_t *start = NULL;
    const uint8_t *end = NULL;
    const uint8_t *margin = NULL;
    size_t margin_length = 0;
    bool valid = token->source_form == XR_LITERAL_BLOCK
                     ? block_source_range(token, &start, &end, &margin, &margin_length, error)
                     : inline_source_range(token, &start, &end, error);
    if (!valid)
        return false;
    size_t capacity = (size_t) (end - start) + 1;
    uint8_t *normalized = (uint8_t *) xr_malloc(capacity);
    if (!normalized) {
        if (error)
            *error = "memory allocation failed";
        return false;
    }
    size_t normalized_length = 0;
    if (token->source_form == XR_LITERAL_BLOCK) {
        valid = normalize_block(start, end, margin, margin_length, normalized, &normalized_length,
                                error);
    } else {
        normalized_length = (size_t) (end - start);
        memcpy(normalized, start, normalized_length);
    }
    if (!valid) {
        xr_free(normalized);
        return false;
    }
    if (!decode_escapes) {
        normalized[normalized_length] = 0;
        out->bytes = normalized;
        out->length = normalized_length;
        return true;
    }
    uint8_t *decoded = (uint8_t *) xr_malloc(normalized_length + 1);
    size_t decoded_length = 0;
    valid = decoded &&
            xr_escaped_bytes_decode(normalized, normalized_length, decoded, &decoded_length, error);
    xr_free(normalized);
    if (!valid) {
        xr_free(decoded);
        if (!decoded && error)
            *error = "memory allocation failed";
        return false;
    }
    decoded[decoded_length] = 0;
    out->bytes = decoded;
    out->length = decoded_length;
    return true;
}

void xr_quoted_payload_free(XrQuotedPayload *payload) {
    if (!payload)
        return;
    xr_free(payload->bytes);
    payload->bytes = NULL;
    payload->length = 0;
}
