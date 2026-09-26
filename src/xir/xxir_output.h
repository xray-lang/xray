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
typedef bool (*XrXirByteOutputEntry)(void *context, XrXirOutputStream stream, const char *bytes, size_t length);
typedef struct XrXirOutputSink {
    XrXirByteOutputEntry write;
    void *context;
    size_t byte_limit;
} XrXirOutputSink;
XR_FUNC bool xr_xir_output_render(void *context, const XrXirOutputGroup *group);
#endif
