/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_transport.h - LSP transport layer (stdio)
 *
 * KEY CONCEPT:
 *   Handles the base protocol layer of LSP:
 *   - Content-Length header parsing
 *   - Message framing over stdio
 */

#ifndef XLSP_TRANSPORT_H
#define XLSP_TRANSPORT_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>
#include "../../base/xdefs.h"

// Transport context
typedef struct XrLspTransport {
    FILE *in;
    FILE *out;
    char *read_buffer;
    size_t buffer_size;
    int in_fd;              // Input file descriptor (for poll)
    
    // Partial message state (for non-blocking reads)
    char *partial_header;   // Accumulated header data
    size_t partial_len;     // Length of partial data
    int pending_content_length;  // Content-Length if header complete
} XrLspTransport;

// Initialize transport with stdio
XR_FUNC XrLspTransport *xlsp_transport_stdio(void);

// Free transport
XR_FUNC void xlsp_transport_free(XrLspTransport *t);

// Read one LSP message (caller must free returned string)
// Returns NULL on EOF or error
// NOTE: This is a BLOCKING read
XR_FUNC char *xlsp_transport_read(XrLspTransport *t, size_t *out_len);

// Write one LSP message
// Automatically adds Content-Length header
XR_FUNC void xlsp_transport_write(XrLspTransport *t, const char *json, size_t len);

// Get input file descriptor for polling
// Returns -1 if not available
XR_FUNC int xlsp_transport_get_fd(XrLspTransport *t);

// Check if there's data available to read (non-blocking)
// Returns: 1 = data available, 0 = no data, -1 = error
XR_FUNC int xlsp_transport_has_data(XrLspTransport *t);

// Try to read one LSP message without blocking
// Returns: message string (caller must free), or NULL if no complete message
// Sets *out_len to message length if not NULL
// Sets *would_block to true if read would block (no complete message yet)
XR_FUNC char *xlsp_transport_try_read(XrLspTransport *t, size_t *out_len, bool *would_block);

#endif // XLSP_TRANSPORT_H
