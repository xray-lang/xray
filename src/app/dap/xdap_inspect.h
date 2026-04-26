/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_inspect.h - Variable and stack inspection for debugging
 *
 * KEY CONCEPT:
 *   Provides inspection of VM runtime state:
 *   - Stack frame enumeration
 *   - Local variable inspection
 *   - Expression evaluation
 *   - Object expansion
 */

#ifndef XDAP_INSPECT_H
#define XDAP_INSPECT_H

#include "xdap_controller.h"
#include "../../base/xjson.h"
#include "../../runtime/value/xvalue.h"

// Forward declarations
typedef struct XrCoroutine XrCoroutine;
typedef struct XrBcCallFrame XrBcCallFrame;

// ============================================================================
// Stack Frames
// ============================================================================

// Get stack frames for a coroutine
// Returns: JSON array of stack frame objects
// frame_id_out: receives the next frame ID to use
XR_FUNC XrJsonValue *xdap_inspect_stack_frames(XdapController *ctrl, XrCoroutine *coro,
                                               int *frame_id_out);

// Get frame info for a specific frame
XR_FUNC bool xdap_inspect_get_frame_info(XdapController *ctrl, XrCoroutine *coro, int frame_idx,
                                         const char **out_name, const char **out_source,
                                         int *out_line);

// ============================================================================
// Variables
// ============================================================================

// Get variables for a variable reference
// Returns: JSON array of variable objects
XR_FUNC XrJsonValue *xdap_inspect_variables(XdapController *ctrl, int var_ref);

// Get local variables for a frame
XR_FUNC XrJsonValue *xdap_inspect_locals(XdapController *ctrl, XrCoroutine *coro, int frame_idx);

// Get closure upvalues for a frame
XR_FUNC XrJsonValue *xdap_inspect_upvalues(XdapController *ctrl, XrCoroutine *coro, int frame_idx);

// Get global variables
XR_FUNC XrJsonValue *xdap_inspect_globals(XdapController *ctrl);

// ============================================================================
// Expression Evaluation
// ============================================================================

// Evaluate an expression in frame context
// Returns: result string (caller must free), or NULL on error
XR_FUNC char *xdap_inspect_evaluate(XdapController *ctrl, const char *expression, int frame_idx);

// Evaluate with expandable result support
// out_var_ref: set to >0 if result is expandable (array, map, object, instance)
XR_FUNC char *xdap_inspect_evaluate_ex(XdapController *ctrl, const char *expression, int frame_idx,
                                       int *out_var_ref);

#endif  // XDAP_INSPECT_H
