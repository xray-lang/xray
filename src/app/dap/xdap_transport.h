/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_transport.h - DAP transport layer (xpoll-based non-blocking I/O)
 *
 * KEY CONCEPT:
 *   Non-blocking transport for DAP protocol, enabling:
 *   - Responsive pause/interrupt during VM execution
 *   - Proper event-driven architecture
 *   - Support for both stdio and TCP transports
 *
 * DESIGN:
 *   Based on xpoll.h for cross-platform I/O multiplexing.
 *   Similar architecture to xlsp_transport but with wakeup support.
 */

#ifndef XDAP_TRANSPORT_H
#define XDAP_TRANSPORT_H

#include <stddef.h>
#include <stdbool.h>
#include "../../base/xpoll.h"
#include "../../base/xdefs.h"

// ============================================================================
// Transport Types
// ============================================================================

typedef enum {
    XDAP_TRANSPORT_STDIO,
    XDAP_TRANSPORT_TCP_SERVER,   // Listen and accept
    XDAP_TRANSPORT_TCP_CLIENT,   // Connect to remote
} XdapTransportType;

typedef struct XdapTransport {
    XdapTransportType type;
    bool connected;
    
    // Poll subsystem (for non-blocking I/O)
    XrPoll poll;
    bool poll_initialized;
    
    // File descriptors
    int read_fd;            // Input fd (stdin or client socket)
    int write_fd;           // Output fd (stdout or client socket)
    int listen_fd;          // Server socket (TCP server mode only)
    
    // Read buffer (for message framing)
    char *read_buf;
    size_t read_len;        // Data in buffer
    size_t read_cap;        // Buffer capacity
    
    // Partial header parsing state
    int pending_content_length;  // -1 = not yet parsed
    size_t header_end;           // Position after \r\n\r\n
    
    // Write buffer (for batching writes)
    char *write_buf;
    size_t write_len;
    size_t write_cap;
    
    // TCP server info
    int listen_port;        // Actual port (0 = use assigned port)
} XdapTransport;

// ============================================================================
// Lifecycle
// ============================================================================

// Create stdio transport (for IDE extension)
XR_FUNC XdapTransport *xdap_transport_stdio(void);

// Create TCP server transport (for remote debugging)
// port=0 means auto-assign port
XR_FUNC XdapTransport *xdap_transport_tcp_server(int port);

// Create TCP client transport (connect to debug server)
XR_FUNC XdapTransport *xdap_transport_tcp_connect(const char *host, int port);

// Free transport
XR_FUNC void xdap_transport_free(XdapTransport *t);

// ============================================================================
// Non-blocking I/O API
// ============================================================================

// Poll for events
// Returns: number of events (0 = timeout, -1 = error)
// timeout_ms: -1 = infinite, 0 = immediate, >0 = milliseconds
XR_FUNC int xdap_transport_poll(XdapTransport *t, int timeout_ms);

// Try to read one complete DAP message (non-blocking)
// Returns: message content (caller must free), or NULL if no complete message
// Sets *would_block to true if no complete message available yet
XR_FUNC char *xdap_transport_try_read(XdapTransport *t, size_t *out_len, bool *would_block);

// Write a DAP message (adds Content-Length header)
// This is synchronous but should be fast for small messages
XR_FUNC void xdap_transport_write(XdapTransport *t, const char *json, size_t len);

// Wakeup blocked poll (call from another thread/context)
// Used to interrupt poll when VM needs to send events
XR_FUNC void xdap_transport_wakeup(XdapTransport *t);

// ============================================================================
// Status
// ============================================================================

// Check if connected
XR_FUNC bool xdap_transport_is_connected(XdapTransport *t);

// Get the actual listen port (for TCP server)
XR_FUNC int xdap_transport_get_port(XdapTransport *t);

// Get read fd for external poll integration
XR_FUNC int xdap_transport_get_fd(XdapTransport *t);

#endif // XDAP_TRANSPORT_H
