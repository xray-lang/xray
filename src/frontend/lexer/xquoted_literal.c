/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xquoted_literal.c - Metered quoted-literal extraction and escape decoding
 */
#include "xquoted_literal.h"
#include "../../base/xutf8.h"
#include <string.h>

static XrQuotedStatus quoted_status(XrCompileState *state) {
    switch (xr_compile_state_status(state)) {
        case XR_COMPILE_RESOURCE_OK: return XR_QUOTED_OK;
        case XR_COMPILE_RESOURCE_BUDGET: return XR_QUOTED_BUDGET;
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_QUOTED_OUT_OF_MEMORY;
        default: return XR_QUOTED_BAD_ARGUMENT;
    }
}

static XrQuotedStatus quoted_syntax(XrCompileState *state, const char **error, const char *message) {
    XrQuotedStatus status = quoted_status(state);
    if (status != XR_QUOTED_OK) return status;
    if (error) *error = message;
    return XR_QUOTED_SYNTAX;
}

/* A source read and a destination write each cost one byte before access. */
static bool quoted_read(XrCompileState *state, const uint8_t *p, uint8_t *value) {
    if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return false;
    *value = *p;
    return true;
}

static bool quoted_write(XrCompileState *state, uint8_t *p, uint8_t value) {
    if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return false;
    *p = value;
    return true;
}

static int hex_value(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

XrQuotedStatus xr_compile_escaped_bytes_decode(XrCompileState *state, const uint8_t *src, size_t length,
                                       uint8_t *dst, size_t *out_length, const char **error) {
    if (!state || !out_length || (length && (!src || !dst))) {
        xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return quoted_status(state);
    }
    if (quoted_status(state) != XR_QUOTED_OK) return quoted_status(state);
    size_t out = 0;
    for (size_t i = 0; i < length; ++i) {
        uint8_t c;
        if (!quoted_read(state, src + i, &c)) return quoted_status(state);
        if (c != '\\' || i + 1 == length) {
            if (!quoted_write(state, dst + out++, c)) return quoted_status(state);
            continue;
        }
        uint8_t escaped;
        if (!quoted_read(state, src + ++i, &escaped)) return quoted_status(state);
        switch (escaped) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '\\': case '"': case '\'': case '$': case '`': c = escaped; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '0': c = '\0'; break;
            case 'x': {
                if (length - i <= 2)
                    return quoted_syntax(state, error, "byte escape must use exactly two hexadecimal digits");
                uint8_t high, low;
                if (!quoted_read(state, src + i + 1, &high) ||
                    !quoted_read(state, src + i + 2, &low)) return quoted_status(state);
                int h = hex_value(high), l = hex_value(low);
                if (h < 0 || l < 0)
                    return quoted_syntax(state, error, "byte escape must use exactly two hexadecimal digits");
                c = (uint8_t) ((h << 4) | l);
                i += 2;
                break;
            }
            case 'u': {
                uint32_t cp = 0;
                uint8_t next = 0;
                if (i + 1 < length && !quoted_read(state, src + i + 1, &next))
                    return quoted_status(state);
                if (next == '{') {
                    size_t p = i + 2;
                    unsigned digits = 0;
                    bool closed = false;
                    while (p < length) {
                        uint8_t digit;
                        if (!quoted_read(state, src + p, &digit)) return quoted_status(state);
                        if (digit == '}') { closed = true; break; }
                        int h = hex_value(digit);
                        if (h < 0) return quoted_syntax(state, error, "invalid hex digit in unicode escape");
                        if (digits == 6)
                            return quoted_syntax(state, error, "unicode escape must contain at most 6 hex digits");
                        cp = (cp << 4) | (uint32_t) h;
                        ++digits;
                        ++p;
                    }
                    if (!digits) return quoted_syntax(state, error, "unicode escape requires at least one hex digit");
                    if (!closed) return quoted_syntax(state, error, "unterminated unicode escape");
                    i = p;
                } else {
                    if (length - i <= 4)
                        return quoted_syntax(state, error, "unicode escape must use exactly four hexadecimal digits (\\uXXXX) or \\u{...}");
                    for (size_t n = 1; n <= 4; ++n) {
                        uint8_t digit = next;
                        if (n != 1 && !quoted_read(state, src + i + n, &digit)) return quoted_status(state);
                        int h = hex_value(digit);
                        if (h < 0)
                            return quoted_syntax(state, error, "unicode escape must use exactly four hexadecimal digits (\\uXXXX) or \\u{...}");
                        cp = (cp << 4) | (uint32_t) h;
                    }
                    i += 4;
                }
                if (!xr_unicode_is_scalar(cp))
                    return quoted_syntax(state, error, "unicode escape must be a valid Unicode scalar value");
                size_t bytes = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                if (xr_compile_state_work(state, bytes) != XR_COMPILE_RESOURCE_OK) return quoted_status(state);
                int encoded = xr_utf8_encode(cp, (char *) dst + out);
                out += (size_t) encoded;
                continue;
            }
            default:
                if (!quoted_write(state, dst + out++, '\\')) return quoted_status(state);
                c = escaped;
                break;
        }
        if (!quoted_write(state, dst + out++, c)) return quoted_status(state);
    }
    *out_length = out;
    return XR_QUOTED_OK;
}

static XrQuotedStatus quoted_range(XrCompileState *state, const Token *token,
                                   const uint8_t **start, const uint8_t **end,
                                   const uint8_t **margin, size_t *margin_length,
                                   const char **error) {
    const uint8_t *text = (const uint8_t *) token->start;
    size_t length = (size_t) token->length;
    size_t prefix = (size_t) token->prefix_length;
    size_t quotes = (size_t) token->quote_count;
    if (prefix > length || quotes > length - prefix)
        return quoted_syntax(state, error, "invalid quoted-literal boundary");
    if (token->source_form == XR_LITERAL_INLINE) {
        if (quotes == 2) { *start = *end = text + length; return XR_QUOTED_OK; }
        if (quotes != 1 || length - prefix < 2)
            return quoted_syntax(state, error, "invalid inline quoted-literal boundary");
        uint8_t first, last;
        if (!quoted_read(state, text + prefix, &first) || !quoted_read(state, text + length - 1, &last))
            return quoted_status(state);
        if (first != '"' || last != '"')
            return quoted_syntax(state, error, "invalid inline quoted-literal boundary");
        *start = text + prefix + 1;
        *end = text + length - 1;
        return XR_QUOTED_OK;
    }
    size_t body = prefix + quotes;
    if (!quotes || quotes > length - body || body >= length)
        return quoted_syntax(state, error, "invalid block quoted-literal boundary");
    uint8_t c;
    if (!quoted_read(state, text + body, &c)) return quoted_status(state);
    if (c == '\r' && body + 1 < length) {
        if (!quoted_read(state, text + body + 1, &c)) return quoted_status(state);
        if (c != '\n') return quoted_syntax(state, error, "block opening delimiter must be followed by a newline");
        body += 2;
    } else if (c == '\n') ++body;
    else return quoted_syntax(state, error, "block opening delimiter must be followed by a newline");
    size_t close = length - quotes, line = close;
    while (line > body) {
        if (!quoted_read(state, text + line - 1, &c)) return quoted_status(state);
        if (c == '\n') break;
        --line;
    }
    for (size_t p = line; p < close; ++p) {
        if (!quoted_read(state, text + p, &c)) return quoted_status(state);
        if (c != ' ' && c != '\t') return quoted_syntax(state, error, "block closing delimiter indentation is invalid");
    }
    size_t content_end = line;
    if (content_end > body) {
        if (!quoted_read(state, text + content_end - 1, &c)) return quoted_status(state);
        if (c != '\n') return quoted_syntax(state, error, "block closing delimiter must begin on its own line");
        --content_end;
        if (content_end > body) {
            if (!quoted_read(state, text + content_end - 1, &c)) return quoted_status(state);
            if (c == '\r') --content_end;
        }
    }
    *start = text + body;
    *end = text + content_end;
    *margin = text + line;
    *margin_length = close - line;
    return XR_QUOTED_OK;
}

static XrQuotedStatus normalize_block(XrCompileState *state, const uint8_t *start,
                                      const uint8_t *end, const uint8_t *margin, size_t margin_length,
                                      uint8_t *out, size_t *out_length, const char **error) {
    const uint8_t *p = start;
    size_t dst = 0;
    bool at_line_start = true;
    while (p < end) {
        uint8_t c;
        if (at_line_start) {
            const uint8_t *line_end = p;
            while (line_end < end) {
                if (!quoted_read(state, line_end, &c)) return quoted_status(state);
                if (c == '\n' || c == '\r') break;
                ++line_end;
            }
            if (line_end > p) {
                if ((size_t) (line_end - p) < margin_length)
                    return quoted_syntax(state, error, "every non-empty block line must begin with the closing margin");
                for (size_t n = 0; n < margin_length; ++n) {
                    uint8_t a, b;
                    if (!quoted_read(state, p + n, &a) || !quoted_read(state, margin + n, &b)) return quoted_status(state);
                    if (a != b) return quoted_syntax(state, error, "every non-empty block line must begin with the closing margin");
                }
                p += margin_length;
            }
            at_line_start = false;
        }
        if (p >= end) break;
        if (!quoted_read(state, p++, &c)) return quoted_status(state);
        if (c == '\r') {
            if (p == end) return quoted_syntax(state, error, "bare carriage return is not allowed in a block literal");
            if (!quoted_read(state, p++, &c)) return quoted_status(state);
            if (c != '\n') return quoted_syntax(state, error, "bare carriage return is not allowed in a block literal");
        }
        if (!quoted_write(state, out + dst++, c)) return quoted_status(state);
        at_line_start = c == '\n';
    }
    *out_length = dst;
    return XR_QUOTED_OK;
}

XrQuotedStatus xr_compile_quoted_payload_decode(XrCompileState *state, const Token *token,
                                        bool decode_escapes, XrQuotedPayload *out, const char **error) {
    if (!state || !token || !out || out->bytes || out->length || !token->start || token->length < 0 ||
        token->prefix_length < 0 || token->quote_count < 0 || token->quoted_kind == XR_QUOTED_NONE ||
        (token->source_form != XR_LITERAL_INLINE && token->source_form != XR_LITERAL_BLOCK)) {
        xr_compile_state_fail(state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return quoted_status(state);
    }
    if (quoted_status(state) != XR_QUOTED_OK) return quoted_status(state);
    const uint8_t *start = NULL, *end = NULL, *margin = NULL;
    size_t margin_length = 0;
    XrQuotedStatus status = quoted_range(state, token, &start, &end, &margin, &margin_length, error);
    if (status != XR_QUOTED_OK) return status;
    size_t length = (size_t) (end - start);
    void *memory = NULL;
    if (xr_compile_state_alloc(state, length + 1, &memory) != XR_COMPILE_RESOURCE_OK) return quoted_status(state);
    uint8_t *normalized = memory;
    if (token->source_form == XR_LITERAL_BLOCK)
        status = normalize_block(state, start, end, margin, margin_length, normalized, &length, error);
    else if (xr_compile_state_copy(state, normalized, start, length) != XR_COMPILE_RESOURCE_OK)
        status = quoted_status(state);
    if (status != XR_QUOTED_OK) { xr_compile_state_free(normalized); return status; }
    uint8_t *result = normalized;
    if (decode_escapes) {
        memory = NULL;
        if (xr_compile_state_alloc(state, length + 1, &memory) != XR_COMPILE_RESOURCE_OK) {
            xr_compile_state_free(normalized);
            return quoted_status(state);
        }
        result = memory;
        status = xr_compile_escaped_bytes_decode(state, normalized, length, result, &length, error);
        xr_compile_state_free(normalized);
        if (status != XR_QUOTED_OK) { xr_compile_state_free(result); return status; }
    }
    if (!quoted_write(state, result + length, 0)) {
        xr_compile_state_free(result);
        return quoted_status(state);
    }
    out->bytes = result;
    out->length = length;
    return XR_QUOTED_OK;
}

void xr_quoted_payload_free(XrQuotedPayload *payload) {
    if (!payload) return;
    xr_compile_state_free(payload->bytes);
    payload->bytes = NULL;
    payload->length = 0;
}
