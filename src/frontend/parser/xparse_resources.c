/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_resources.c - Metered parser reads and arena payload transfer
 */
#include "xparse_internal.h"
#include "../../base/xutf8.h"

int xr_parser_grow_capacity(Parser *parser, int old) {
    if (!xr_parser_healthy(parser)) return 0;
    if (old < 0 || old > INT_MAX / 2) {
        xr_compile_state_fail(parser->state, old < 0 ? XR_COMPILE_RESOURCE_BAD_ARGUMENT : XR_COMPILE_RESOURCE_BUDGET);
        return 0;
    }
    return old ? old * 2 : 4;
}

char xr_parser_read_byte(const Parser *parser, const char *address) {
    if (xr_compile_state_work(parser->state, 1) != XR_COMPILE_RESOURCE_OK) return 0;
    return *address;
}

size_t xr_parser_string_length(const Parser *parser, const char *text) {
    size_t length = 0;
    (void) xr_compile_state_string_length(parser->state, text, &length);
    return length;
}

int xr_parser_compare_bytes(const Parser *parser, const void *left, const void *right, size_t length) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < length; ++i) {
        if (xr_compile_state_work(parser->state, 2) != XR_COMPILE_RESOURCE_OK) return 0;
        unsigned char first = a[i], second = b[i];
        if (first != second) return first < second ? -1 : 1;
    }
    return 0;
}

int xr_parser_compare_string_n(const Parser *parser, const char *left, const char *right, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (xr_compile_state_work(parser->state, 2) != XR_COMPILE_RESOURCE_OK) return 0;
        unsigned char a = (unsigned char) left[i], b = (unsigned char) right[i];
        if (a != b) return a < b ? -1 : 1;
        if (!a) return 0;
    }
    return 0;
}

int xr_parser_compare_string(const Parser *parser, const char *left, const char *right) {
    return xr_parser_compare_string_n(parser, left, right, SIZE_MAX);
}

void *xr_parser_find_byte(const Parser *parser, const void *text, int value, size_t length) {
    const unsigned char *bytes = text;
    for (size_t i = 0; i < length; ++i) {
        if (xr_compile_state_work(parser->state, 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        if (bytes[i] == (unsigned char) value) return (void *) (bytes + i);
    }
    return NULL;
}

char *xr_parser_copy_string_n(const Parser *parser, char *destination, const char *source, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (xr_compile_state_work(parser->state, 1) != XR_COMPILE_RESOURCE_OK) return NULL;
        char value = source[i];
        destination[i] = value;
        if (!value) {
            if (xr_compile_state_zero(parser->state, destination + i + 1, length - i - 1) != XR_COMPILE_RESOURCE_OK)
                return NULL;
            break;
        }
    }
    return destination;
}

char *xr_parser_copy_string(const Parser *parser, char *destination, const char *source) {
    size_t length = xr_parser_string_length(parser, source);
    if (!xr_parser_healthy(parser)) return NULL;
    return xr_compile_state_copy(parser->state, destination, source, length + 1) == XR_COMPILE_RESOURCE_OK
        ? destination : NULL;
}

static int parser_utf8_read(void *context, const uint8_t *address, uint8_t *output) {
    XrCompileState *state = context;
    if (xr_compile_state_work(state, 1) != XR_COMPILE_RESOURCE_OK) return 0;
    *output = *address;
    return 1;
}

bool xr_parser_utf8_validate(const Parser *parser, const char *text, size_t length) {
    XrUtf8ScanResult scan;
    return xr_utf8_scan_strict_read((const uint8_t *) text, length, parser_utf8_read, parser->state, &scan)
        && scan.error == XR_UTF8_OK;
}

int xr_parser_utf8_decode(const Parser *parser, const char *text, size_t length, uint32_t *output) {
    XrUtf8Step step;
    if (!xr_utf8_core_decode_step_read((const uint8_t *) text, length, parser_utf8_read, parser->state, &step))
        return 0;
    if (step.error != XR_UTF8_OK) { *output = XR_UNICODE_INVALID; return 1; }
    *output = step.scalar;
    return (int) step.consumed;
}

XrQuotedStatus xr_parser_decode_quoted(Parser *parser, const Token *token, bool escapes,
                                        XrParsedQuoted *output, const char **error) {
    if (!output || output->bytes || output->length) {
        xr_compile_state_fail(parser->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return XR_QUOTED_BAD_ARGUMENT;
    }
    XrQuotedPayload temporary = {0};
    XrQuotedStatus status = xr_compile_quoted_payload_decode(parser->state, token, escapes, &temporary, error);
    if (status != XR_QUOTED_OK) return status;
    char *owned = ast_strndup(parser->compiler_session, (const char *) temporary.bytes, temporary.length);
    size_t length = temporary.length;
    xr_quoted_payload_free(&temporary);
    if (!owned) {
        switch (xr_compile_state_status(parser->state)) {
            case XR_COMPILE_RESOURCE_BUDGET: return XR_QUOTED_BUDGET;
            case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: return XR_QUOTED_OUT_OF_MEMORY;
            default: return XR_QUOTED_BAD_ARGUMENT;
        }
    }
    output->bytes = (const uint8_t *) owned;
    output->length = length;
    return XR_QUOTED_OK;
}

static int numeric_digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Parse integer literal (supports multiple bases and underscore separators)
// Formats: decimal (123), hex (0xFF), binary (0b1010), octal (0o755)
ParsedIntLiteral xr_parser_integer_literal(Parser *parser, const char *start, int length) {
    if (!xr_parser_healthy(parser)) return (ParsedIntLiteral){0};
    ParsedIntLiteral out = {0};
    int base = 10;
    int pos = 0;

    if (length >= 2 && xr_parser_read_byte(parser, start) == '0') {
        if (!xr_parser_healthy(parser)) return (ParsedIntLiteral){0};
        char prefix = xr_parser_read_byte(parser, start + (1));
        if (!xr_parser_healthy(parser)) return (ParsedIntLiteral){0};
        if (prefix == 'x' || prefix == 'X') {
            base = 16;
            pos = 2;
        } else if (prefix == 'b' || prefix == 'B') {
            base = 2;
            pos = 2;
        } else if (prefix == 'o' || prefix == 'O') {
            base = 8;
            pos = 2;
        }
    }

    for (int i = pos; i < length; i++) {
        char byte = xr_parser_read_byte(parser, start + i);
        if (!xr_parser_healthy(parser)) return (ParsedIntLiteral){0};
        if (byte == '_') continue;
        int digit = numeric_digit_value(byte);
        if (digit < 0 || digit >= base)
            continue;
        if (out.bits > (UINT64_MAX - (uint64_t) digit) / (uint64_t) base) {
            out.bits = UINT64_MAX;
            out.overflows_i64 = true;
            out.overflows_u64 = true;
            continue;
        }
        out.bits = out.bits * (uint64_t) base + (uint64_t) digit;
    }

    out.overflows_i64 = out.overflows_u64 || out.bits > (uint64_t) INT64_MAX;
    return out;
}


char *xr_parser_token_string(Parser *parser, const Token *token) {
    if (!xr_parser_healthy(parser) || !token || !token->length) return NULL;
    if (token->length < 0) {
        xr_compile_state_fail(parser->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return NULL;
    }
    return ast_strndup(parser->compiler_session, token->start, (size_t) token->length);
}

bool xr_parser_step(const Parser *parser) {
    return xr_parser_healthy(parser) && xr_compile_state_work(parser->state, 1) == XR_COMPILE_RESOURCE_OK;
}
