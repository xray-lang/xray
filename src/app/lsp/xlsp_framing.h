/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_framing.h - Strict byte-oriented LSP frame boundaries
 */
#ifndef XLSP_FRAMING_H
#define XLSP_FRAMING_H
#include "../../base/xdefs.h"
#include <stddef.h>
#define XLSP_FRAME_MAX_BODY (64 * 1024 * 1024)
#define XLSP_FRAME_MAX_HEADER 8192u
typedef enum XlspFrameStatus {XLSP_FRAME_OK, XLSP_FRAME_PARTIAL, XLSP_FRAME_ERROR} XlspFrameStatus;
/* No allocation. Error and incomplete headers preserve both outputs. A full
 * valid header publishes its exact boundary/length even if its body is partial.
 * Exactly one Content-Length is mandatory; an explicit charset must be utf-8. */
XR_FUNC XlspFrameStatus xlsp_frame_read(const char *, size_t, size_t *header_end, int *content_length);
XR_FUNC int xlsp_frame_header(char *, size_t, size_t body_length);
#endif // XLSP_FRAMING_H
