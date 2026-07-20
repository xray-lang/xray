/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xquoted_literal.h - Shared quoted-literal payload extraction
 */

#ifndef XQUOTED_LITERAL_H
#define XQUOTED_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xlex.h"
#include "../../base/xdefs.h"

typedef struct XrQuotedPayload {
    uint8_t *bytes;
    size_t length;
} XrQuotedPayload;

// Extracts the semantic payload boundary, normalizes block CRLF to LF,
// removes the closing-line margin, and optionally decodes escapes.
XR_FUNC bool xr_quoted_payload_decode(const Token *token, bool decode_escapes, XrQuotedPayload *out,
                                      const char **error);
XR_FUNC void xr_quoted_payload_free(XrQuotedPayload *payload);

// Decodes escape sequences (\n \t \xHH \uXXXX \u{...} ...) from src into dst.
// dst must hold at least length bytes (decoded output never exceeds source
// length). Single escape decoder shared by plain literals and template-string
// parts so both surfaces stay in sync. On failure returns false and points
// *error at a static message.
XR_FUNC bool xr_escaped_bytes_decode(const uint8_t *src, size_t length, uint8_t *dst,
                                     size_t *out_length, const char **error);

#endif  // XQUOTED_LITERAL_H
