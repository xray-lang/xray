/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_output.h - Atomic rendering of typed output groups
 *
 * KEY CONCEPT:
 *   All validation and rendering precedes the single byte-sink operation.
 */
#ifndef XXIR_OUTPUT_H
#define XXIR_OUTPUT_H
#include "xxir_call.h"
typedef XrXirOutputStatus (*XrXirByteOutputEntry)(void *context, XrXirOutputStream stream, const char *bytes, size_t length);
typedef struct XrXirOutputSink {
    uint32_t abi_version, reserved;
    XrXirByteOutputEntry write;
    void *context;
    size_t byte_limit;
} XrXirOutputSink;
XR_FUNC XrXirOutputStatus xr_xir_output_render(void *context, const XrXirOutputGroup *group);
/* Decimal, shortest round-trip float or true/false spelling of one scalar into a
 * caller buffer of at least XR_XIR_SCALAR_TEXT_BYTES bytes; false for any other type. */
#define XR_XIR_SCALAR_TEXT_BYTES 32
XR_FUNC bool xr_xir_scalar_text(const XrXirValue *value, char *buffer, const char **bytes, size_t *length);
#endif
