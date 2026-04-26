/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xframing.h - Content-Length framing protocol (LSP/DAP/MCP shared)
 *
 * KEY CONCEPT:
 *   Buffer-level frame parsing and header writing for the base protocol
 *   used by LSP, DAP, and MCP. Each transport manages its own I/O
 *   strategy (blocking/non-blocking, fd/socket); this module only
 *   handles the "Content-Length: N\r\n\r\n<body>" wire format.
 */

#ifndef XFRAMING_H
#define XFRAMING_H

#include <stddef.h>
#include "xdefs.h"

/* Result of trying to parse a frame from a buffer. */
typedef enum {
    XR_FRAME_OK,      /* Complete frame found */
    XR_FRAME_PARTIAL, /* Need more data */
    XR_FRAME_ERROR    /* Malformed header (missing/invalid Content-Length) */
} XrFrameStatus;

/*
 * Try to parse one Content-Length frame from buf[0..buf_len).
 *
 * On XR_FRAME_OK:
 *   *header_end     = offset just past "\r\n\r\n"
 *   *content_length = the parsed Content-Length value (>= 0)
 *   Caller can extract body at buf[header_end .. header_end+content_length).
 *
 * On XR_FRAME_PARTIAL:
 *   Header block ("\r\n\r\n") not yet seen, or body bytes not fully arrived.
 *   *header_end and *content_length are left unchanged.
 *
 * On XR_FRAME_ERROR:
 *   Header block was found but Content-Length is missing or invalid.
 */
XR_FUNC XrFrameStatus xr_frame_parse(const char *buf, size_t buf_len, size_t *header_end,
                                     int *content_length);

/*
 * Write "Content-Length: <body_len>\r\n\r\n" into buf.
 * Returns the number of bytes written (excluding NUL terminator),
 * or -1 if buf_size is too small.
 */
XR_FUNC int xr_frame_write_header(char *buf, size_t buf_size, size_t body_len);

#endif  // XFRAMING_H
