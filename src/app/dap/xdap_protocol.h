/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_protocol.h - DAP protocol message handling
 *
 * KEY CONCEPT:
 *   Handles all DAP protocol messages:
 *   - Request parsing and response building
 *   - Event generation
 *   - JSON serialization using xjson (src/base)
 */

#ifndef XDAP_PROTOCOL_H
#define XDAP_PROTOCOL_H

#include "xdap_controller.h"
#include "../../base/xjson.h"

// ============================================================================
// Message Handling
// ============================================================================

// Handle incoming DAP request
// Returns: true if session should continue, false to exit
XR_FUNC bool xdap_handle_message(XdapController *ctrl, const char *json, size_t len);

// ============================================================================
// Response/Event Sending
// ============================================================================

// Send response to a request
XR_FUNC void xdap_send_response(XdapController *ctrl, int request_seq, const char *command,
                                bool success, XrJsonValue *body, const char *error_message);

// Send event
XR_FUNC void xdap_send_event(XdapController *ctrl, const char *event, XrJsonValue *body);

// ============================================================================
// Common Events
// ============================================================================

// Send "initialized" event (after initialize response)
XR_FUNC void xdap_send_initialized_event(XdapController *ctrl);

// Send "stopped" event
XR_FUNC void xdap_send_stopped_event(XdapController *ctrl, const char *reason, int thread_id);

// Send "terminated" event
XR_FUNC void xdap_send_terminated_event(XdapController *ctrl);

// Send "exited" event
XR_FUNC void xdap_send_exited_event(XdapController *ctrl, int exit_code);

// Send "output" event (for logpoints and debug console)
XR_FUNC void xdap_send_output_event(XdapController *ctrl, const char *category, const char *output);

// ============================================================================
// Main Loop
// ============================================================================

// Run the DAP event loop
// Returns: exit code
XR_FUNC int xdap_run(XdapController *ctrl);

#endif  // XDAP_PROTOCOL_H
