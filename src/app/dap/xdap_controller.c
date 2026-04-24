/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_controller.c - DAP Debug Controller implementation
 *
 * NOTE: All breakpoints and variable references are managed by xdap_debug.
 * This file only handles controller state and VM control.
 */

#include "xdap_controller.h"
#include "xdap_debug.h"
#include "../../runtime/xisolate_api.h"
#include "../../base/xmalloc.h"
#include "xray_isolate.h"
#include "../../coro/xcoroutine.h"
#include "../../module/xmodule.h"
#include "../../vm/xvm_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Lifecycle
// ============================================================================

XdapController *xdap_controller_new(XdapTransport *transport) {
    XdapController *ctrl = xr_calloc(1, sizeof(XdapController));
    if (!ctrl) return NULL;

    ctrl->transport = transport;
    ctrl->vm_state = XDAP_VM_INITIALIZING;
    ctrl->seq = 1;

    return ctrl;
}

void xdap_controller_free(XdapController *ctrl) {
    if (!ctrl) return;

    // Free debug proto if not already freed
    if (ctrl->debug_proto && ctrl->isolate) {
        xr_free_code(ctrl->isolate, ctrl->debug_proto);
        ctrl->debug_proto = NULL;
    }

    // Free program info
    xr_free(ctrl->program_path);
    if (ctrl->program_args) {
        for (int i = 0; i < ctrl->arg_count; i++) {
            xr_free(ctrl->program_args[i]);
        }
        xr_free(ctrl->program_args);
    }

    // Free isolate if owned by controller
    if (ctrl->isolate) {
        xr_debug_free(ctrl->isolate);  // Free debug state first
        xray_isolate_delete(ctrl->isolate);
        ctrl->isolate = NULL;
    }

    // Free transport
    xdap_transport_free(ctrl->transport);

    xr_free(ctrl);
}

// ============================================================================
// VM Control
// ============================================================================

bool xdap_controller_launch(XdapController *ctrl, const char *program,
                             char **args, int arg_count, bool stop_on_entry) {
    if (!ctrl || !program) return false;

    // Store program info
    ctrl->program_path = xr_strdup(program);
    if (!ctrl->program_path) return false;

    if (args && arg_count > 0) {
        ctrl->program_args = xr_calloc((size_t)arg_count, sizeof(char *));
        if (!ctrl->program_args) {
            xr_free(ctrl->program_path);
            ctrl->program_path = NULL;
            return false;
        }
        ctrl->arg_count = arg_count;
        for (int i = 0; i < arg_count; i++) {
            ctrl->program_args[i] = xr_strdup(args[i]);
            if (!ctrl->program_args[i]) {
                // Cleanup on failure
                for (int j = 0; j < i; j++) {
                    xr_free(ctrl->program_args[j]);
                }
                xr_free(ctrl->program_args);
                xr_free(ctrl->program_path);
                ctrl->program_args = NULL;
                ctrl->program_path = NULL;
                ctrl->arg_count = 0;
                return false;
            }
        }
    }

    // Create isolate
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    params.trace_execution = false;

    ctrl->isolate = xray_isolate_new(&params);
    if (!ctrl->isolate) {
        return false;
    }

    // Set userdata to controller for callbacks
    xray_isolate_set_userdata(ctrl->isolate, ctrl);

    // Initialize multicore runtime
    xr_multicore_init(ctrl->isolate, 0);

    // Set script info
    xray_isolate_set_script_info(ctrl->isolate, program, arg_count, args);

    // Initialize module system
    xr_module_system_init_with_script(ctrl->isolate, program);

    // Initialize and enable debug state
    xr_debug_init(ctrl->isolate);
    xr_debug_enable(ctrl->isolate, true);

    // Set initial state based on stop_on_entry
    if (stop_on_entry) {
        ctrl->step_mode = XDAP_CMD_STEP_IN;  // Step in will stop at first line
    }

    ctrl->vm_state = XDAP_VM_PAUSED;  // Wait for configurationDone

    return true;
}

void xdap_controller_continue(XdapController *ctrl) {
    if (!ctrl) return;
    ctrl->step_mode = XDAP_CMD_NONE;
    if (ctrl->isolate) {
        xr_debug_continue(ctrl->isolate);
        xr_debug_clear_var_refs(ctrl->isolate);
    }
}

void xdap_controller_step_in(XdapController *ctrl) {
    if (!ctrl) return;
    ctrl->step_mode = XDAP_CMD_STEP_IN;
    if (ctrl->isolate) {
        xr_debug_step_in(ctrl->isolate);
        xr_debug_clear_var_refs(ctrl->isolate);
    }
}

void xdap_controller_step_out(XdapController *ctrl) {
    if (!ctrl) return;
    ctrl->step_mode = XDAP_CMD_STEP_OUT;
    if (ctrl->isolate) {
        xr_debug_step_out(ctrl->isolate);
        xr_debug_clear_var_refs(ctrl->isolate);
    }
}

void xdap_controller_step_over(XdapController *ctrl) {
    if (!ctrl) return;
    ctrl->step_mode = XDAP_CMD_STEP_OVER;
    if (ctrl->isolate) {
        xr_debug_step_over(ctrl->isolate);
        xr_debug_clear_var_refs(ctrl->isolate);
    }
}

void xdap_controller_pause(XdapController *ctrl) {
    if (!ctrl) return;
    atomic_store(&ctrl->pending_cmd, XDAP_CMD_PAUSE);
    atomic_store(&ctrl->cmd_pending, true);

    // Wakeup transport poll (if blocked)
    if (ctrl->transport) {
        xdap_transport_wakeup(ctrl->transport);
    }
}

// Atomic restart: build a fresh isolate *before* tearing down the old
// one. If the new launch fails (bad program path, OOM, etc.) we roll
// back to the previous session so the user's debugging context is
// preserved — they can retry the restart or manually terminate.
bool xdap_controller_restart(XdapController *ctrl) {
    if (!ctrl || !ctrl->program_path) return false;

    // 1. Snapshot program info as independent copies. `launch()` will
    //    strdup them again, so we release these locals when done.
    char *program = xr_strdup(ctrl->program_path);
    if (!program) return false;

    char **args_copy = NULL;
    int arg_count = ctrl->arg_count;
    if (ctrl->program_args && arg_count > 0) {
        args_copy = xr_calloc((size_t)arg_count, sizeof(char *));
        if (!args_copy) { xr_free(program); return false; }
        for (int i = 0; i < arg_count; i++) {
            args_copy[i] = xr_strdup(ctrl->program_args[i]);
            if (!args_copy[i]) {
                for (int j = 0; j < i; j++) xr_free(args_copy[j]);
                xr_free(args_copy);
                xr_free(program);
                return false;
            }
        }
    }

    // 2. Park the old session on the stack. `launch()` overwrites these
    //    fields unconditionally, so we MUST null them first — otherwise
    //    the `ctrl->program_path = xr_strdup(...)` at the top of launch
    //    would silently leak our snapshot.
    XrayIsolate *old_isolate      = ctrl->isolate;
    XrProto     *old_debug_proto  = ctrl->debug_proto;
    char        *old_program_path = ctrl->program_path;
    char       **old_program_args = ctrl->program_args;
    int          old_arg_count    = ctrl->arg_count;

    ctrl->isolate       = NULL;
    ctrl->debug_proto   = NULL;
    ctrl->program_path  = NULL;
    ctrl->program_args  = NULL;
    ctrl->arg_count     = 0;

    // Also clear any stopped-session pointers now — whether restart
    // succeeds or fails, the old stop point is no longer meaningful.
    ctrl->program_launched  = false;
    ctrl->stopped_coro      = NULL;
    ctrl->stopped_coro_id   = 0;
    ctrl->stopped_path      = NULL;
    ctrl->stopped_line      = 0;
    ctrl->step_mode         = XDAP_CMD_NONE;
    atomic_store(&ctrl->cmd_pending, false);

    // 3. Build the new isolate. At this point the old one is still
    //    intact in `old_isolate`; we have not touched its memory.
    bool ok = xdap_controller_launch(ctrl, program, args_copy, arg_count, false);

    // 4. Release the local copies — launch either succeeded (and has
    //    its own strdup'd copies) or failed (and any partial state is
    //    cleaned up in the rollback path below).
    xr_free(program);
    if (args_copy) {
        for (int i = 0; i < arg_count; i++) xr_free(args_copy[i]);
        xr_free(args_copy);
    }

    if (ok) {
        // 5a. Success — now (and only now) is it safe to free the
        //     old isolate and its program info.
        if (old_isolate) {
            xr_debug_free(old_isolate);
            xr_multicore_destroy(old_isolate);
            xray_isolate_delete(old_isolate);
        }
        xr_free(old_program_path);
        if (old_program_args) {
            for (int i = 0; i < old_arg_count; i++) xr_free(old_program_args[i]);
            xr_free(old_program_args);
        }

        ctrl->vm_state  = XDAP_VM_RUNNING;
        ctrl->configured = true;
        return true;
    }

    // 5b. Failure — `launch()` may have partially populated ctrl->*
    //     (e.g. program_path set but isolate creation failed at line
    //     117). Tear down anything it left behind, then put the old
    //     session back in place so the DAP UI keeps a live target.
    xr_free(ctrl->program_path);
    if (ctrl->program_args) {
        for (int i = 0; i < ctrl->arg_count; i++) xr_free(ctrl->program_args[i]);
        xr_free(ctrl->program_args);
    }
    if (ctrl->isolate) {
        xr_debug_free(ctrl->isolate);
        xr_multicore_destroy(ctrl->isolate);
        xray_isolate_delete(ctrl->isolate);
    }

    ctrl->isolate       = old_isolate;
    ctrl->debug_proto   = old_debug_proto;
    ctrl->program_path  = old_program_path;
    ctrl->program_args  = old_program_args;
    ctrl->arg_count     = old_arg_count;
    return false;
}

void xdap_controller_terminate(XdapController *ctrl) {
    if (!ctrl) return;
    atomic_store(&ctrl->pending_cmd, XDAP_CMD_TERMINATE);
    atomic_store(&ctrl->cmd_pending, true);
    ctrl->vm_state = XDAP_VM_TERMINATED;

    // Cleanup
    if (ctrl->isolate) {
        xr_multicore_destroy(ctrl->isolate);
    }
}

// ============================================================================
// Coroutine/Thread Lookup
// ============================================================================

XrCoroutine *xdap_find_coro(XdapController *ctrl, int thread_id) {
    if (!ctrl || !ctrl->isolate) return NULL;

    // First check: stopped coroutine (most common case during debugging)
    if (ctrl->stopped_coro && ctrl->stopped_coro_id == thread_id) {
        return ctrl->stopped_coro;
    }

    // Thread ID 1 typically maps to main coroutine
    if (thread_id == 1 || thread_id <= 0) {
        if (xr_isolate_get_main_coro(ctrl->isolate)) {
            return xr_isolate_get_main_coro(ctrl->isolate);
        }
    }

    // Check if main coroutine has matching ID
    if (xr_isolate_get_main_coro(ctrl->isolate) && xr_isolate_get_main_coro(ctrl->isolate)->id == thread_id) {
        return xr_isolate_get_main_coro(ctrl->isolate);
    }

    // Fall back to stopped coro or main coro (log for diagnostics)
    fprintf(stderr, "[DAP] xdap_find_coro: thread_id=%d not found, falling back\n", thread_id);
    if (ctrl->stopped_coro) return ctrl->stopped_coro;
    return xr_isolate_get_main_coro(ctrl->isolate);
}

int xdap_coro_to_thread_id(XrCoroutine *coro) {
    if (!coro) return 1;  // Default to main thread
    return coro->id > 0 ? coro->id : 1;
}

// ============================================================================
// VM Hook Interface
// ============================================================================

void xdap_on_stopped(XdapController *ctrl, XdapStopReason reason,
                      XrCoroutine *coro, const char *path, int line) {
    if (!ctrl) return;

    ctrl->vm_state = XDAP_VM_PAUSED;
    ctrl->stop_reason = reason;
    ctrl->stopped_coro = coro;
    ctrl->stopped_coro_id = xdap_coro_to_thread_id(coro);
    ctrl->stopped_path = path;
    ctrl->stopped_line = line;
    ctrl->step_mode = XDAP_CMD_NONE;
}

