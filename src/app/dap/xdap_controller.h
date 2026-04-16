/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_controller.h - DAP Debug Controller (core state machine)
 *
 * KEY CONCEPT:
 *   Centralized state management for debugging session.
 *   All breakpoints and variable references are managed by xdap_debug module.
 *
 * ARCHITECTURE:
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │                  XdapController                             │
 *   │  ┌─────────────┐  ┌──────────────┐  ┌───────────────┐      │
 *   │  │ VM State    │  │ Step State   │  │ Pending Cmd   │      │
 *   │  │ (running/   │  │ (mode/depth) │  │ (pause req)   │      │
 *   │  │  paused/    │  │              │  │               │      │
 *   │  │  terminated)│  │              │  │               │      │
 *   │  └─────────────┘  └──────────────┘  └───────────────┘      │
 *   │                                                             │
 *   │  All breakpoints & VarRefs → managed by xdap_debug module   │
 *   └─────────────────────────────────────────────────────────────┘
 */

#ifndef XDAP_CONTROLLER_H
#define XDAP_CONTROLLER_H

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xdap_transport.h"

#include "../../base/xforward_decl.h"
#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif

typedef struct XrProto XrProto;

// ============================================================================
// Command Types (from DAP IO to VM)
// ============================================================================

typedef enum {
    XDAP_CMD_NONE = 0,
    XDAP_CMD_CONTINUE,
    XDAP_CMD_STEP_IN,
    XDAP_CMD_STEP_OUT,
    XDAP_CMD_STEP_OVER,
    XDAP_CMD_PAUSE,
    XDAP_CMD_TERMINATE,
} XdapCommand;

// ============================================================================
// VM State
// ============================================================================

typedef enum {
    XDAP_VM_INITIALIZING,   // Isolate created, not started
    XDAP_VM_RUNNING,        // VM executing bytecode
    XDAP_VM_PAUSED,         // VM stopped at breakpoint/step/pause
    XDAP_VM_TERMINATED,     // VM finished or terminated
} XdapVMState;

// ============================================================================
// Stop Reason
// ============================================================================

typedef enum {
    XDAP_STOP_NONE = 0,
    XDAP_STOP_ENTRY,        // Stopped on entry
    XDAP_STOP_BREAKPOINT,   // Hit a breakpoint
    XDAP_STOP_STEP,         // Step completed
    XDAP_STOP_PAUSE,        // User requested pause
    XDAP_STOP_EXCEPTION,    // Exception thrown
} XdapStopReason;

// ============================================================================
// Debug Controller (main state container)
// ============================================================================

typedef struct XdapController {
    // Transport (owned)
    XdapTransport *transport;
    
    // VM target
    XrayIsolate *isolate;
    char *program_path;
    char **program_args;
    int arg_count;
    XrProto *debug_proto;    // Keep alive for resume
    
    // Session state
    XdapVMState vm_state;
    bool initialized;       // DAP initialized
    bool configured;        // DAP configurationDone received
    bool program_launched;  // Program has been initially launched (for launch vs resume)
    int seq;                // DAP message sequence number
    
    // Stopped state
    XdapStopReason stop_reason;
    XrCoroutine *stopped_coro;
    int stopped_coro_id;    // DAP threadId
    const char *stopped_path;
    int stopped_line;
    
    // Step state (step_mode tracks current step command for stop notification)
    XdapCommand step_mode;
    
    // Pending command (from IO to VM, accessed cross-thread)
    _Atomic XdapCommand pending_cmd;
    _Atomic bool cmd_pending;
    
} XdapController;

// ============================================================================
// Lifecycle
// ============================================================================

// Create controller
XR_FUNC XdapController *xdap_controller_new(XdapTransport *transport);

// Free controller (also frees transport)
XR_FUNC void xdap_controller_free(XdapController *ctrl);

// ============================================================================
// VM Control
// ============================================================================

// Launch program for debugging
XR_FUNC bool xdap_controller_launch(XdapController *ctrl, const char *program,
                             char **args, int arg_count, bool stop_on_entry);

// Continue/step execution
XR_FUNC void xdap_controller_continue(XdapController *ctrl);
XR_FUNC void xdap_controller_step_in(XdapController *ctrl);
XR_FUNC void xdap_controller_step_out(XdapController *ctrl);
XR_FUNC void xdap_controller_step_over(XdapController *ctrl);

// Request pause (sets flag, checked by VM hook)
XR_FUNC void xdap_controller_pause(XdapController *ctrl);

// Restart (cleanup + re-launch with same program)
XR_FUNC bool xdap_controller_restart(XdapController *ctrl);

// Terminate
XR_FUNC void xdap_controller_terminate(XdapController *ctrl);

// ============================================================================
// Coroutine/Thread Lookup
// ============================================================================

// Find coroutine by DAP threadId
XR_FUNC XrCoroutine *xdap_find_coro(XdapController *ctrl, int thread_id);

// Get DAP threadId for coroutine
XR_FUNC int xdap_coro_to_thread_id(XrCoroutine *coro);

// ============================================================================
// VM Hook Interface
// ============================================================================

// Notify controller that VM stopped
XR_FUNC void xdap_on_stopped(XdapController *ctrl, XdapStopReason reason,
                      XrCoroutine *coro, const char *path, int line);

#endif // XDAP_CONTROLLER_H
