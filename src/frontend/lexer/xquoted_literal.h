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
#include "../../base/xcompile_state.h"

typedef enum XrQuotedStatus {
    XR_QUOTED_OK,
    XR_QUOTED_SYNTAX,
    XR_QUOTED_BAD_ARGUMENT,
    XR_QUOTED_OUT_OF_MEMORY,
    XR_QUOTED_MEMORY_LIMIT, XR_QUOTED_OVERFLOW, XR_QUOTED_CANCELLED,
    XR_QUOTED_DEADLINE, XR_QUOTED_CLOCK_FAILURE
} XrQuotedStatus;

typedef struct XrQuotedInput {
    XrCompileState *state;
    XrCompileRequestControl *request; /* Borrowed only for one decoding operation. */
} XrQuotedInput;

typedef struct XrQuotedPayload {
    uint8_t *bytes;
    size_t length;
} XrQuotedPayload;

// Extracts the semantic payload boundary, normalizes block CRLF to LF,
// removes the closing-line margin, and optionally decodes escapes. The caller
// supplies an empty output; all failures preserve it. Resource failures never
// become syntax diagnostics. Payload storage keeps its memory owner alive; request control never escapes.
XR_FUNC XrQuotedStatus xr_compile_quoted_payload_decode(const XrQuotedInput *input, const Token *token, bool decode_escapes, XrQuotedPayload *out,
                                      const char **error);
XR_FUNC void xr_quoted_payload_free(XrQuotedPayload *payload);

// Decodes escape sequences (\n \t \xHH \uXXXX \u{...} ...) from src into dst.
// dst must hold at least length bytes (decoded output never exceeds source
// length). Single escape decoder shared by plain literals and template-string
// parts so both surfaces stay in sync. Syntax failure points
// *error at a static message. Resource failure is retained in state.
XR_FUNC XrQuotedStatus xr_compile_escaped_bytes_decode(const XrQuotedInput *input, const uint8_t *src, size_t length, uint8_t *dst,
                                     size_t *out_length, const char **error);

#endif  // XQUOTED_LITERAL_H
