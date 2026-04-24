/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_protocol.c - DAP protocol message handling implementation
 */

#include "xdap_protocol.h"
#include "xdap_inspect.h"
#include "xdap_debug.h"
#include "../../runtime/xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../../coro/xcoroutine.h"
#include "../../runtime/closure/xclosure.h"
#include "../../runtime/xexec_frame.h"
#include "../../runtime/value/xchunk.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>   // chdir
#include "../../vm/xvm_internal.h"
#include "xray.h"

// ============================================================================
// JSON Helpers
// ============================================================================

static inline XrJsonValue *json_get(XrJsonValue *obj, const char *key) {
    return xjson_get(obj, key);
}

static inline double json_number(XrJsonValue *v) {
    if (!v || v->type != XR_JSON_NUMBER) return 0;
    return v->is_integer ? (double)v->as.integer : v->as.number;
}

static inline const char *json_string(XrJsonValue *v) {
    return v && v->type == XR_JSON_STRING ? v->as.string : NULL;
}

static inline bool json_bool(XrJsonValue *v) {
    return v && v->type == XR_JSON_BOOL && v->as.boolean;
}

// ============================================================================
// Message Sending
// ============================================================================

void xdap_send_response(XdapController *ctrl, int request_seq,
                         const char *command, bool success,
                         XrJsonValue *body, const char *error_message) {
    XrJsonValue *resp = xjson_new_object();
    xjson_object_set(resp, "seq", xjson_new_number(ctrl->seq++));
    xjson_object_set(resp, "type", xjson_new_string("response"));
    xjson_object_set(resp, "request_seq", xjson_new_number(request_seq));
    xjson_object_set(resp, "success", xjson_new_bool(success));
    xjson_object_set(resp, "command", xjson_new_string(command));

    if (body) {
        xjson_object_set(resp, "body", body);
    }
    if (error_message) {
        xjson_object_set(resp, "message", xjson_new_string(error_message));
    }

    size_t len = 0;
    char *json = xjson_stringify(resp, &len);
    xdap_transport_write(ctrl->transport, json, len);
    xr_free(json);
    xjson_free(resp);
}

void xdap_send_event(XdapController *ctrl, const char *event, XrJsonValue *body) {
    XrJsonValue *evt = xjson_new_object();
    xjson_object_set(evt, "seq", xjson_new_number(ctrl->seq++));
    xjson_object_set(evt, "type", xjson_new_string("event"));
    xjson_object_set(evt, "event", xjson_new_string(event));

    if (body) {
        xjson_object_set(evt, "body", body);
    }

    size_t len = 0;
    char *json = xjson_stringify(evt, &len);
    xdap_transport_write(ctrl->transport, json, len);
    xr_free(json);
    xjson_free(evt);
}

// ============================================================================
// Common Events
// ============================================================================

void xdap_send_initialized_event(XdapController *ctrl) {
    xdap_send_event(ctrl, "initialized", NULL);
}

void xdap_send_stopped_event(XdapController *ctrl, const char *reason, int thread_id) {
    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "reason", xjson_new_string(reason));
    xjson_object_set(body, "threadId", xjson_new_number(thread_id));
    xjson_object_set(body, "allThreadsStopped", xjson_new_bool(true));
    xdap_send_event(ctrl, "stopped", body);
}

void xdap_send_terminated_event(XdapController *ctrl) {
    xdap_send_event(ctrl, "terminated", NULL);
}

void xdap_send_exited_event(XdapController *ctrl, int exit_code) {
    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "exitCode", xjson_new_number(exit_code));
    xdap_send_event(ctrl, "exited", body);
}

void xdap_send_output_event(XdapController *ctrl, const char *category,
                             const char *output) {
    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "category", xjson_new_string(category));
    xjson_object_set(body, "output", xjson_new_string(output));
    xdap_send_event(ctrl, "output", body);
}

// ============================================================================
// Request Handlers
// ============================================================================

static void handle_initialize(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;

    XrJsonValue *body = xjson_new_object();

    // Capabilities
    xjson_object_set(body, "supportsConfigurationDoneRequest",
                          xjson_new_bool(true));
    xjson_object_set(body, "supportsConditionalBreakpoints", xjson_new_bool(true));
    xjson_object_set(body, "supportsHitConditionalBreakpoints", xjson_new_bool(true));
    xjson_object_set(body, "supportsLogPoints", xjson_new_bool(true));
    xjson_object_set(body, "supportsFunctionBreakpoints", xjson_new_bool(true));
    xjson_object_set(body, "supportsTerminateRequest", xjson_new_bool(true));
    xjson_object_set(body, "supportsRestartRequest", xjson_new_bool(true));
    xjson_object_set(body, "supportsSetVariable", xjson_new_bool(true));
    xjson_object_set(body, "supportsEvaluateForHovers", xjson_new_bool(true));
    xjson_object_set(body, "supportsDisassembleRequest", xjson_new_bool(true));
    xjson_object_set(body, "supportTerminateDebuggee", xjson_new_bool(true));
    xjson_object_set(body, "supportsPauseRequest", xjson_new_bool(true));
    xjson_object_set(body, "supportsExceptionInfoRequest", xjson_new_bool(true));

    // Exception breakpoint filters
    XrJsonValue *filters = xjson_new_array();

    XrJsonValue *uncaught = xjson_new_object();
    xjson_object_set(uncaught, "filter", xjson_new_string("uncaught"));
    xjson_object_set(uncaught, "label", xjson_new_string("Uncaught Exceptions"));
    xjson_object_set(uncaught, "default", xjson_new_bool(true));
    xjson_array_push(filters, uncaught);

    XrJsonValue *caught = xjson_new_object();
    xjson_object_set(caught, "filter", xjson_new_string("caught"));
    xjson_object_set(caught, "label", xjson_new_string("Caught Exceptions"));
    xjson_object_set(caught, "default", xjson_new_bool(false));
    xjson_array_push(filters, caught);

    xjson_object_set(body, "exceptionBreakpointFilters", filters);

    xdap_send_response(ctrl, seq, "initialize", true, body, NULL);
    ctrl->initialized = true;

    xdap_send_initialized_event(ctrl);
}

// Apply optional cwd / env launch arguments before starting the program.
// Because xray-dap embeds the debuggee in its own process, changing the
// process-wide working directory and environment here is what the client
// expects (VS Code's DebugAdapter model assumes launch-time cwd/env apply
// to the debuggee — in our case, to this process).
//
// Returns false and writes a failure response if any step fails; the
// caller must stop processing in that case.
static bool apply_launch_cwd_env(XdapController *ctrl, int seq, XrJsonValue *args) {
    // cwd (optional)
    const char *cwd = json_string(json_get(args, "cwd"));
    if (cwd && cwd[0] != '\0') {
        if (chdir(cwd) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to chdir to '%s'", cwd);
            xdap_send_response(ctrl, seq, "launch", false, NULL, msg);
            return false;
        }
    }

    // env (optional): object of string -> string. An explicit null value
    // means "unset the variable", mirroring VS Code's launch.json semantics.
    XrJsonValue *env = json_get(args, "env");
    if (env && env->type == XR_JSON_OBJECT) {
        for (int i = 0; i < env->as.object.count; i++) {
            XrJsonMember *m = &env->as.object.members[i];
            if (!m->key) continue;
            XrJsonValue *v = m->value;
            if (v && v->type == XR_JSON_STRING && v->as.string) {
                setenv(m->key, v->as.string, 1);
            } else if (!v || v->type == XR_JSON_NULL) {
                unsetenv(m->key);
            }
            // Non-string, non-null values are silently skipped — DAP spec
            // requires strings here, but we don't want to kill the session
            // over a client bug.
        }
    }
    return true;
}

static void handle_launch(XdapController *ctrl, int seq, XrJsonValue *args) {
    XrJsonValue *program_val = json_get(args, "program");
    const char *program = json_string(program_val);

    if (!program) {
        xdap_send_response(ctrl, seq, "launch", false, NULL, "Missing 'program' argument");
        return;
    }

    // Honour cwd / env before touching the program — if this fails we bail
    // out with a descriptive error, identical to how other adapters behave.
    if (!apply_launch_cwd_env(ctrl, seq, args)) {
        return;
    }

    // Get arguments
    XrJsonValue *args_array = json_get(args, "args");
    char **argv = NULL;
    int argc = 0;

    if (args_array && xjson_is_array(args_array)) {
        argc = xjson_array_len(args_array);
        if (argc > 0) {
            argv = xr_calloc((size_t)argc, sizeof(char *));
            if (!argv) {
                xdap_send_response(ctrl, seq, "launch", false, NULL, "Out of memory");
                return;
            }
            for (int i = 0; i < argc; i++) {
                XrJsonValue *arg = xjson_array_get(args_array, i);
                if (arg && arg->type == XR_JSON_STRING) {
                    argv[i] = xr_strdup(arg->as.string);
                    if (!argv[i]) {
                        // Cleanup on failure
                        for (int j = 0; j < i; j++) xr_free(argv[j]);
                        xr_free(argv);
                        xdap_send_response(ctrl, seq, "launch", false, NULL, "Out of memory");
                        return;
                    }
                }
            }
        }
    }

    // Stop on entry
    XrJsonValue *stop_on_entry = json_get(args, "stopOnEntry");
    bool should_stop = json_bool(stop_on_entry);

    // Launch
    if (!xdap_controller_launch(ctrl, program, argv, argc, should_stop)) {
        xdap_send_response(ctrl, seq, "launch", false, NULL, "Failed to launch program");

        // Cleanup
        for (int i = 0; i < argc; i++) xr_free(argv[i]);
        xr_free(argv);
        return;
    }

    // Cleanup (controller made copies)
    for (int i = 0; i < argc; i++) xr_free(argv[i]);
    xr_free(argv);

    xdap_send_response(ctrl, seq, "launch", true, NULL, NULL);
}

// DAP 'attach' request.
//
// For xray, "attach" is operationally the same as "launch": the IDE has
// already opened a TCP connection to `xray dap --port N`, and that `xray dap`
// process itself hosts the Isolate that runs the program. There is no
// separate process to attach to. We therefore accept 'attach' with a
// `program` argument and treat it exactly like launch — but *without* the
// stopOnEntry / restart semantics tightly bound to launch.
//
// Accepting the request (rather than returning `Unknown command`) is what
// lets the VS Code side's attach-style `launch.json` entry succeed; the
// alternative would confuse users.
static void handle_attach(XdapController *ctrl, int seq, XrJsonValue *args) {
    XrJsonValue *program_val = json_get(args, "program");
    const char *program = json_string(program_val);

    if (!apply_launch_cwd_env(ctrl, seq, args)) {
        return;
    }

    // If the IDE gave us a program path, reuse the launch path.
    if (program) {
        XrJsonValue *args_array = json_get(args, "args");
        char **argv = NULL;
        int argc = 0;
        if (args_array && xjson_is_array(args_array)) {
            argc = xjson_array_len(args_array);
            if (argc > 0) {
                argv = xr_calloc((size_t)argc, sizeof(char *));
                if (!argv) {
                    xdap_send_response(ctrl, seq, "attach", false, NULL, "Out of memory");
                    return;
                }
                for (int i = 0; i < argc; i++) {
                    XrJsonValue *arg = xjson_array_get(args_array, i);
                    if (arg && arg->type == XR_JSON_STRING) {
                        argv[i] = xr_strdup(arg->as.string);
                        if (!argv[i]) {
                            for (int j = 0; j < i; j++) xr_free(argv[j]);
                            xr_free(argv);
                            xdap_send_response(ctrl, seq, "attach", false, NULL, "Out of memory");
                            return;
                        }
                    }
                }
            }
        }

        if (!xdap_controller_launch(ctrl, program, argv, argc, false)) {
            xdap_send_response(ctrl, seq, "attach", false, NULL, "Failed to start program");
            for (int i = 0; i < argc; i++) xr_free(argv[i]);
            xr_free(argv);
            return;
        }
        for (int i = 0; i < argc; i++) xr_free(argv[i]);
        xr_free(argv);
    }
    // If no program is given, the debuggee is expected to already be running
    // (driven elsewhere); just acknowledge success — setBreakpoints /
    // configurationDone etc. will still work against ctrl->isolate once it's
    // wired up by whatever launched this process.

    xdap_send_response(ctrl, seq, "attach", true, NULL, NULL);
}

static void handle_configuration_done(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;
    ctrl->configured = true;
    xdap_send_response(ctrl, seq, "configurationDone", true, NULL, NULL);

    // Start execution
    ctrl->vm_state = XDAP_VM_RUNNING;
}

static void handle_set_breakpoints(XdapController *ctrl, int seq, XrJsonValue *args) {
    XrJsonValue *source = json_get(args, "source");
    XrJsonValue *breakpoints = json_get(args, "breakpoints");

    if (!source) {
        xdap_send_response(ctrl, seq, "setBreakpoints", false, NULL, "Missing 'source'");
        return;
    }

    const char *path = json_string(json_get(source, "path"));

    // Check if isolate exists (required for setting breakpoints)
    if (!ctrl->isolate) {
        xdap_send_response(ctrl, seq, "setBreakpoints", false, NULL,
                           "Cannot set breakpoints before launch");
        return;
    }

    // Clear existing breakpoints for this file (directly use debug API)
    if (path) {
        xr_debug_clear_breakpoints(ctrl->isolate, path);
    }

    XrJsonValue *result_bps = xjson_new_array();

    int bp_count = breakpoints ? xjson_array_len(breakpoints) : 0;
    for (int i = 0; i < bp_count; i++) {
        XrJsonValue *bp = xjson_array_get(breakpoints, i);
        int line = (int)json_number(json_get(bp, "line"));
        const char *condition = json_string(json_get(bp, "condition"));
        const char *log_message = json_string(json_get(bp, "logMessage"));
        const char *hit_condition = json_string(json_get(bp, "hitCondition"));

        // Directly use debug API
        int id = xr_debug_add_breakpoint_ex(ctrl->isolate, path, line,
                                             condition, log_message, hit_condition);

        XrJsonValue *result_bp = xjson_new_object();
        xjson_object_set(result_bp, "id", xjson_new_number(id));
        xjson_object_set(result_bp, "verified", xjson_new_bool(id > 0));
        xjson_object_set(result_bp, "line", xjson_new_number(line));
        xjson_array_push(result_bps, result_bp);
    }

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "breakpoints", result_bps);

    xdap_send_response(ctrl, seq, "setBreakpoints", true, body, NULL);
}

static void handle_set_function_breakpoints(XdapController *ctrl, int seq, XrJsonValue *args) {
    XrJsonValue *breakpoints = json_get(args, "breakpoints");

    // Check if isolate exists
    if (!ctrl->isolate) {
        xdap_send_response(ctrl, seq, "setFunctionBreakpoints", false, NULL,
                           "Cannot set function breakpoints before launch");
        return;
    }

    // Clear existing function breakpoints (directly use debug API)
    xr_debug_clear_function_breakpoints(ctrl->isolate);

    XrJsonValue *result_bps = xjson_new_array();

    int bp_count = breakpoints ? xjson_array_len(breakpoints) : 0;
    for (int i = 0; i < bp_count; i++) {
        XrJsonValue *bp = xjson_array_get(breakpoints, i);
        const char *name = json_string(json_get(bp, "name"));
        const char *condition = json_string(json_get(bp, "condition"));

        // Directly use debug API
        int id = xr_debug_add_function_breakpoint(ctrl->isolate, name, condition);

        XrJsonValue *result_bp = xjson_new_object();
        xjson_object_set(result_bp, "id", xjson_new_number(id));
        xjson_object_set(result_bp, "verified", xjson_new_bool(id > 0));
        xjson_array_push(result_bps, result_bp);
    }

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "breakpoints", result_bps);

    xdap_send_response(ctrl, seq, "setFunctionBreakpoints", true, body, NULL);
}

static void handle_set_exception_breakpoints(XdapController *ctrl, int seq, XrJsonValue *args) {
    XrJsonValue *filters = json_get(args, "filters");

    bool break_uncaught = false;
    bool break_caught = false;

    // First pass: compute effective toggles.
    int filter_count = filters && xjson_is_array(filters)
                     ? xjson_array_len(filters) : 0;
    for (int i = 0; i < filter_count; i++) {
        XrJsonValue *filter = xjson_array_get(filters, i);
        const char *filter_str = json_string(filter);
        if (!filter_str) continue;
        if (strcmp(filter_str, "uncaught") == 0) break_uncaught = true;
        else if (strcmp(filter_str, "caught") == 0) break_caught = true;
        else if (strcmp(filter_str, "all") == 0) {
            break_uncaught = true;
            break_caught = true;
        }
    }

    // Forward to debug state (single source of truth)
    if (ctrl->isolate) {
        xr_debug_set_exception_breakpoints(ctrl->isolate, break_uncaught, break_caught);
    }

    // DAP spec (1.66+): the response MUST mirror the incoming filters array
    // with one Breakpoint per filter, carrying `verified` so the IDE can
    // style unsupported filters or show a warning bubble. Returning an
    // empty array was legal but caused VS Code 1.92+ to show every filter
    // as "not verified" regardless of what we actually support.
    XrJsonValue *result_bps = xjson_new_array();
    for (int i = 0; i < filter_count; i++) {
        XrJsonValue *filter = xjson_array_get(filters, i);
        const char *filter_str = json_string(filter);
        bool verified = filter_str && (
            strcmp(filter_str, "uncaught") == 0 ||
            strcmp(filter_str, "caught")   == 0 ||
            strcmp(filter_str, "all")      == 0);

        XrJsonValue *bp = xjson_new_object();
        xjson_object_set(bp, "verified", xjson_new_bool(verified));
        if (!verified && filter_str) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Unknown exception filter '%s'", filter_str);
            xjson_object_set(bp, "message", xjson_new_string(msg));
        }
        xjson_array_push(result_bps, bp);
    }

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "breakpoints", result_bps);

    xdap_send_response(ctrl, seq, "setExceptionBreakpoints", true, body, NULL);
}

static void handle_threads(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;

    XrJsonValue *threads = xjson_new_array();

    if (ctrl->isolate) {
        // Get all coroutines from the scheduler
        int coro_count = xr_debug_get_coro_count(ctrl->isolate);

        if (coro_count > 0) {
            for (int i = 0; i < coro_count; i++) {
                int coro_id;
                const char *coro_name;
                const char *coro_state;

                if (xr_debug_get_coro_info(ctrl->isolate, i, &coro_id, &coro_name, &coro_state)) {
                    XrJsonValue *thread = xjson_new_object();
                    xjson_object_set(thread, "id", xjson_new_number(coro_id > 0 ? coro_id : 1));

                    // Build thread name with state
                    char name_buf[128];
                    if (coro_name) {
                        snprintf(name_buf, sizeof(name_buf), "%s (%s)", coro_name, coro_state);
                    } else if (i == 0) {
                        snprintf(name_buf, sizeof(name_buf), "main (%s)", coro_state);
                    } else {
                        snprintf(name_buf, sizeof(name_buf), "coroutine-%d (%s)", coro_id, coro_state);
                    }
                    xjson_object_set(thread, "name", xjson_new_string(name_buf));
                    xjson_array_push(threads, thread);
                }
            }
        } else {
            // No scheduler, just add main thread
            XrJsonValue *main_thread = xjson_new_object();
            xjson_object_set(main_thread, "id", xjson_new_number(1));
            xjson_object_set(main_thread, "name", xjson_new_string("main"));
            xjson_array_push(threads, main_thread);
        }
    } else {
        // No isolate, return empty (or default main thread)
        XrJsonValue *main_thread = xjson_new_object();
        xjson_object_set(main_thread, "id", xjson_new_number(1));
        xjson_object_set(main_thread, "name", xjson_new_string("main"));
        xjson_array_push(threads, main_thread);
    }

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "threads", threads);

    xdap_send_response(ctrl, seq, "threads", true, body, NULL);
}

static void handle_stack_trace(XdapController *ctrl, int seq, XrJsonValue *args) {
    int thread_id = (int)json_number(json_get(args, "threadId"));
    int start_frame = (int)json_number(json_get(args, "startFrame"));  // 0 if absent
    int levels = (int)json_number(json_get(args, "levels"));            // 0 if absent

    // Find the coroutine for this thread
    XrCoroutine *coro = xdap_find_coro(ctrl, thread_id);

    XrJsonValue *all_frames = NULL;

    if (coro && ctrl->vm_state == XDAP_VM_PAUSED) {
        int frame_id = 0;
        all_frames = xdap_inspect_stack_frames(ctrl, coro, &frame_id);
    }

    if (!all_frames) {
        all_frames = xjson_new_array();
    }

    int total = xjson_array_len(all_frames);

    // Apply pagination: startFrame + levels
    XrJsonValue *frames = all_frames;
    if (start_frame > 0 || (levels > 0 && levels < total)) {
        frames = xjson_new_array();
        int end = (levels > 0) ? start_frame + levels : total;
        if (end > total) end = total;
        for (int i = start_frame; i < end; i++) {
            xjson_array_push(frames, xjson_array_get(all_frames, i));
        }
    }

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "stackFrames", frames);
    xjson_object_set(body, "totalFrames", xjson_new_number(total));

    xdap_send_response(ctrl, seq, "stackTrace", true, body, NULL);
}

static void handle_scopes(XdapController *ctrl, int seq, XrJsonValue *args) {
    int frame_id = (int)json_number(json_get(args, "frameId"));

    if (!ctrl->isolate) {
        xdap_send_response(ctrl, seq, "scopes", false, NULL, "No active session");
        return;
    }

    XrJsonValue *scopes = xjson_new_array();

    // Resolve frame for variable counts
    XrBcCallFrame *frame = NULL;
    int actual_idx = -1;
    if (ctrl->stopped_coro) {
        actual_idx = ctrl->stopped_coro->vm_ctx.frame_count - 1 - frame_id;
        if (actual_idx >= 0 && actual_idx < ctrl->stopped_coro->vm_ctx.frame_count) {
            frame = &ctrl->stopped_coro->vm_ctx.frames[actual_idx];
        }
    }

    // Locals scope (always present)
    int locals_ref = xr_debug_create_var_ref(ctrl->isolate, XDAP_REF_SCOPE_LOCALS,
                                              frame_id, XR_NULL_VAL);
    int local_count = (frame && frame->closure && frame->closure->proto)
        ? PROTO_LOCVAR_COUNT(frame->closure->proto) : 0;
    XrJsonValue *local = xjson_new_object();
    xjson_object_set(local, "name", xjson_new_string("Locals"));
    xjson_object_set(local, "variablesReference", xjson_new_number(locals_ref));
    xjson_object_set(local, "namedVariables", xjson_new_number(local_count));
    xjson_object_set(local, "expensive", xjson_new_bool(false));
    xjson_array_push(scopes, local);

    // Closure scope (only when frame has upvalues)
    if (frame && frame->closure && frame->closure->upval_count > 0) {
        int upval_ref = xr_debug_create_var_ref(ctrl->isolate,
            XDAP_REF_SCOPE_UPVALUES, frame_id, XR_NULL_VAL);
        XrJsonValue *closure_scope = xjson_new_object();
        xjson_object_set(closure_scope, "name", xjson_new_string("Closure"));
        xjson_object_set(closure_scope, "variablesReference",
            xjson_new_number(upval_ref));
        xjson_object_set(closure_scope, "namedVariables",
            xjson_new_number(frame->closure->upval_count));
        xjson_object_set(closure_scope, "expensive", xjson_new_bool(false));
        xjson_array_push(scopes, closure_scope);
    }

    // Globals scope (always present, marked expensive)
    int globals_ref = xr_debug_create_var_ref(ctrl->isolate, XDAP_REF_SCOPE_GLOBALS,
                                               -1, XR_NULL_VAL);
    XrJsonValue *global = xjson_new_object();
    xjson_object_set(global, "name", xjson_new_string("Globals"));
    xjson_object_set(global, "variablesReference", xjson_new_number(globals_ref));
    xjson_object_set(global, "expensive", xjson_new_bool(true));
    xjson_array_push(scopes, global);

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "scopes", scopes);

    xdap_send_response(ctrl, seq, "scopes", true, body, NULL);
}

static void handle_variables(XdapController *ctrl, int seq, XrJsonValue *args) {
    int var_ref = (int)json_number(json_get(args, "variablesReference"));
    int start = (int)json_number(json_get(args, "start"));    // 0 if absent
    int count = (int)json_number(json_get(args, "count"));    // 0 if absent

    XrJsonValue *all_vars = xdap_inspect_variables(ctrl, var_ref);
    if (!all_vars) all_vars = xjson_new_array();

    // Apply pagination when start/count provided
    XrJsonValue *variables = all_vars;
    int total = xjson_array_len(all_vars);
    if (start > 0 || (count > 0 && count < total)) {
        variables = xjson_new_array();
        int end = (count > 0) ? start + count : total;
        if (end > total) end = total;
        for (int i = start; i < end; i++) {
            xjson_array_push(variables, xjson_array_get(all_vars, i));
        }
    }

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "variables", variables);

    xdap_send_response(ctrl, seq, "variables", true, body, NULL);
}

static void handle_continue(XdapController *ctrl, int seq, XrJsonValue *args) {
    int thread_id = (int)json_number(json_get(args, "threadId"));

    if (ctrl->vm_state != XDAP_VM_PAUSED) {
        xdap_send_response(ctrl, seq, "continue", false, NULL, "Not paused");
        return;
    }

    // Record target thread for future multi-coroutine support
    if (thread_id > 0) ctrl->stopped_coro_id = thread_id;
    xdap_controller_continue(ctrl);
    ctrl->vm_state = XDAP_VM_RUNNING;

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "allThreadsContinued", xjson_new_bool(true));
    xdap_send_response(ctrl, seq, "continue", true, body, NULL);
}

static void handle_next(XdapController *ctrl, int seq, XrJsonValue *args) {
    int thread_id = (int)json_number(json_get(args, "threadId"));
    if (thread_id > 0) ctrl->stopped_coro_id = thread_id;

    if (ctrl->vm_state != XDAP_VM_PAUSED) {
        xdap_send_response(ctrl, seq, "next", false, NULL, "Not paused");
        return;
    }

    xdap_controller_step_over(ctrl);
    ctrl->vm_state = XDAP_VM_RUNNING;

    xdap_send_response(ctrl, seq, "next", true, NULL, NULL);
}

static void handle_step_in(XdapController *ctrl, int seq, XrJsonValue *args) {
    int thread_id = (int)json_number(json_get(args, "threadId"));
    if (thread_id > 0) ctrl->stopped_coro_id = thread_id;

    if (ctrl->vm_state != XDAP_VM_PAUSED) {
        xdap_send_response(ctrl, seq, "stepIn", false, NULL, "Not paused");
        return;
    }

    xdap_controller_step_in(ctrl);
    ctrl->vm_state = XDAP_VM_RUNNING;

    xdap_send_response(ctrl, seq, "stepIn", true, NULL, NULL);
}

static void handle_step_out(XdapController *ctrl, int seq, XrJsonValue *args) {
    int thread_id = (int)json_number(json_get(args, "threadId"));
    if (thread_id > 0) ctrl->stopped_coro_id = thread_id;

    if (ctrl->vm_state != XDAP_VM_PAUSED) {
        xdap_send_response(ctrl, seq, "stepOut", false, NULL, "Not paused");
        return;
    }

    xdap_controller_step_out(ctrl);
    ctrl->vm_state = XDAP_VM_RUNNING;

    xdap_send_response(ctrl, seq, "stepOut", true, NULL, NULL);
}

static void handle_pause(XdapController *ctrl, int seq, XrJsonValue *args) {
    int thread_id = (int)json_number(json_get(args, "threadId"));
    if (thread_id > 0) ctrl->stopped_coro_id = thread_id;

    if (ctrl->vm_state != XDAP_VM_RUNNING) {
        xdap_send_response(ctrl, seq, "pause", false, NULL, "Not running");
        return;
    }

    // Set pause flag (VM will check this)
    xdap_controller_pause(ctrl);

    xdap_send_response(ctrl, seq, "pause", true, NULL, NULL);
}

static void handle_evaluate(XdapController *ctrl, int seq, XrJsonValue *args) {
    const char *expression = json_string(json_get(args, "expression"));
    int frame_id = (int)json_number(json_get(args, "frameId"));

    XrJsonValue *body = xjson_new_object();

    if (expression && ctrl->isolate) {
        int var_ref = 0;
        char *result = xdap_inspect_evaluate_ex(ctrl, expression, frame_id, &var_ref);
        xjson_object_set(body, "result", xjson_new_string(result ? result : "<error>"));
        xjson_object_set(body, "variablesReference", xjson_new_number(var_ref));
        xr_free(result);
    } else {
        xjson_object_set(body, "result", xjson_new_string("<no expression>"));
        xjson_object_set(body, "variablesReference", xjson_new_number(0));
    }

    xdap_send_response(ctrl, seq, "evaluate", true, body, NULL);
}

static void handle_set_variable(XdapController *ctrl, int seq, XrJsonValue *args) {
    int var_ref = (int)json_number(json_get(args, "variablesReference"));
    const char *name = json_string(json_get(args, "name"));
    const char *value = json_string(json_get(args, "value"));

    if (!ctrl->isolate || !name || !value) {
        xdap_send_response(ctrl, seq, "setVariable", false, NULL, "Invalid arguments");
        return;
    }

    char *new_value = xr_debug_set_variable(ctrl->isolate, var_ref, name, value);
    if (!new_value) {
        xdap_send_response(ctrl, seq, "setVariable", false, NULL, "Failed to set variable");
        return;
    }

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "value", xjson_new_string(new_value));
    xr_free(new_value);

    xdap_send_response(ctrl, seq, "setVariable", true, body, NULL);
}

static void handle_disassemble(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;

    if (!ctrl->isolate) {
        xdap_send_response(ctrl, seq, "disassemble", false, NULL, "No active session");
        return;
    }

    // Use frame 0 (top of stack) by default
    int frame_idx = 0;
    XdapDisasmInstr *instrs = NULL;
    int count = 0;

    int rc = xr_debug_get_disassembly(ctrl->isolate, frame_idx, &instrs, &count);
    if (rc != 0 || !instrs) {
        xdap_send_response(ctrl, seq, "disassemble", false, NULL, "Disassembly failed");
        return;
    }

    int current_pc = xr_debug_get_current_pc(ctrl->isolate, frame_idx);

    XrJsonValue *instructions = xjson_new_array();
    for (int i = 0; i < count; i++) {
        XrJsonValue *instr = xjson_new_object();

        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "0x%08x", instrs[i].offset);
        xjson_object_set(instr, "address", xjson_new_string(addr_buf));

        char *text = instrs[i].instruction ? instrs[i].instruction : "???";
        if (instrs[i].comment) {
            char combined[512];
            snprintf(combined, sizeof(combined), "%s  ; %s", text, instrs[i].comment);
            xjson_object_set(instr, "instruction", xjson_new_string(combined));
        } else {
            xjson_object_set(instr, "instruction", xjson_new_string(text));
        }

        if (instrs[i].line > 0) {
            xjson_object_set(instr, "line", xjson_new_number(instrs[i].line));
        }

        // Mark current instruction
        if (instrs[i].offset == current_pc) {
            xjson_object_set(instr, "symbol", xjson_new_string(">>>"));
        }

        xjson_array_push(instructions, instr);
    }

    xr_debug_free_disasm(instrs, count);

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "instructions", instructions);

    xdap_send_response(ctrl, seq, "disassemble", true, body, NULL);
}

static void handle_restart(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;

    if (!xdap_controller_restart(ctrl)) {
        xdap_send_response(ctrl, seq, "restart", false, NULL,
                           ctrl->program_path ? "Failed to re-launch" : "No program to restart");
        ctrl->vm_state = XDAP_VM_TERMINATED;
        return;
    }

    xdap_send_response(ctrl, seq, "restart", true, NULL, NULL);
}

static void handle_terminate(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;

    xdap_controller_terminate(ctrl);

    xdap_send_response(ctrl, seq, "terminate", true, NULL, NULL);
    xdap_send_terminated_event(ctrl);
}

static void handle_disconnect(XdapController *ctrl, int seq, XrJsonValue *args) {
    // DAP spec: disconnect has an optional `terminateDebuggee` field
    // (defaults to true for launch sessions, false for attach). When the
    // client explicitly asks us *not* to kill the debuggee, we only tear
    // down the IDE-facing state and leave the VM alone so the caller can
    // reattach (currently xray-dap and the debuggee share a process, so
    // this is best-effort — we still stop dispatching commands, but do
    // NOT flip vm_state to TERMINATED).
    XrJsonValue *tdb = json_get(args, "terminateDebuggee");
    // Default: terminate. This matches VS Code's default for launch
    // sessions and matches the previous behaviour of this handler.
    bool terminate = tdb ? json_bool(tdb) : true;

    if (terminate) {
        xdap_controller_terminate(ctrl);
        ctrl->vm_state = XDAP_VM_TERMINATED;
    }
    // else: leave vm_state as-is; the outer loop will notice that the IDE
    // disconnected via transport EOF and exit cleanly.

    xdap_send_response(ctrl, seq, "disconnect", true, NULL, NULL);
}

static void handle_exception_info(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;

    if (!ctrl->isolate || ctrl->stop_reason != XDAP_STOP_EXCEPTION) {
        xdap_send_response(ctrl, seq, "exceptionInfo", false, NULL,
                           "Not stopped on an exception");
        return;
    }

    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(ctrl->isolate);
    const char *message = (dbg && dbg->exception_message) ? dbg->exception_message : "<unknown>";
    const char *filter = (dbg && dbg->exception_is_uncaught) ? "uncaught" : "caught";

    // breakMode: "always" | "unhandled" | "userUnhandled" | "never"
    const char *break_mode = (dbg && dbg->exception_is_uncaught) ? "unhandled" : "always";

    XrJsonValue *body = xjson_new_object();
    xjson_object_set(body, "exceptionId", xjson_new_string(filter));
    xjson_object_set(body, "description", xjson_new_string(message));
    xjson_object_set(body, "breakMode", xjson_new_string(break_mode));

    xdap_send_response(ctrl, seq, "exceptionInfo", true, body, NULL);
}

// ============================================================================
// Message Dispatch (table-driven)
// ============================================================================

typedef void (*DapHandler)(XdapController *ctrl, int seq, XrJsonValue *args);

typedef struct {
    const char *command;
    DapHandler  handler;
} DapDispatchEntry;

static const DapDispatchEntry dispatch_table[] = {
    {"initialize",              handle_initialize},
    {"launch",                  handle_launch},
    {"attach",                  handle_attach},
    {"configurationDone",       handle_configuration_done},
    {"setBreakpoints",          handle_set_breakpoints},
    {"setFunctionBreakpoints",  handle_set_function_breakpoints},
    {"setExceptionBreakpoints", handle_set_exception_breakpoints},
    {"threads",                 handle_threads},
    {"stackTrace",              handle_stack_trace},
    {"scopes",                  handle_scopes},
    {"variables",               handle_variables},
    {"continue",                handle_continue},
    {"next",                    handle_next},
    {"stepIn",                  handle_step_in},
    {"stepOut",                 handle_step_out},
    {"pause",                   handle_pause},
    {"evaluate",                handle_evaluate},
    {"setVariable",             handle_set_variable},
    {"exceptionInfo",            handle_exception_info},
    {"disassemble",             handle_disassemble},
    {"restart",                 handle_restart},
    {"terminate",               handle_terminate},
    {"disconnect",              handle_disconnect},
    {NULL,                      NULL}
};

static void dispatch_request(XdapController *ctrl, int seq, const char *cmd, XrJsonValue *args) {
    for (const DapDispatchEntry *e = dispatch_table; e->command; e++) {
        if (strcmp(cmd, e->command) == 0) {
            e->handler(ctrl, seq, args);
            return;
        }
    }
    xdap_send_response(ctrl, seq, cmd, false, NULL, "Unknown command");
}

bool xdap_handle_message(XdapController *ctrl, const char *json, size_t len) {
    XrJsonValue *msg = xjson_parse(json, len);
    if (!msg) {
        // JSON parse failed - log and continue session
        fprintf(stderr, "[DAP] Failed to parse JSON message\n");
        return true;
    }

    XrJsonValue *type = json_get(msg, "type");
    if (type && type->type == XR_JSON_STRING && strcmp(type->as.string, "request") == 0) {
        int seq = (int)json_number(json_get(msg, "seq"));
        const char *cmd = json_string(json_get(msg, "command"));
        XrJsonValue *args = json_get(msg, "arguments");

        if (cmd) {
            dispatch_request(ctrl, seq, cmd, args);
        } else {
            // Missing command field - send error response
            xdap_send_response(ctrl, seq, "unknown", false, NULL, "Missing 'command' field in request");
        }
    }

    xjson_free(msg);

    return ctrl->vm_state != XDAP_VM_TERMINATED;
}

// ============================================================================
// Main Loop
// ============================================================================

// Get stop reason string for DAP event
static const char *stop_reason_str(XdapStopReason reason) {
    switch (reason) {
        case XDAP_STOP_ENTRY: return "entry";
        case XDAP_STOP_BREAKPOINT: return "breakpoint";
        case XDAP_STOP_STEP: return "step";
        case XDAP_STOP_PAUSE: return "pause";
        case XDAP_STOP_EXCEPTION: return "exception";
        default: return "breakpoint";
    }
}

int xdap_run(XdapController *ctrl) {
    if (!ctrl || !ctrl->transport) return 1;

    XdapVMState prev_state = ctrl->vm_state;

    while (ctrl->vm_state != XDAP_VM_TERMINATED) {
        // Determine poll timeout based on VM state
        int timeout_ms;
        if (ctrl->vm_state == XDAP_VM_RUNNING) {
            timeout_ms = 0;  // Non-blocking when VM should run
        } else {
            timeout_ms = 100;  // Wait for messages when paused
        }

        // Check for DAP messages
        int poll_result = xdap_transport_poll(ctrl->transport, timeout_ms);

        if (poll_result > 0 || timeout_ms == 0) {
            // Try to read a message
            size_t len;
            bool would_block;
            char *msg = xdap_transport_try_read(ctrl->transport, &len, &would_block);

            if (msg) {
                if (!xdap_handle_message(ctrl, msg, len)) {
                    xr_free(msg);
                    break;  // Session ended
                }
                xr_free(msg);
            } else if (!would_block) {
                // EOF or error
                break;
            }
        }

        // Detect state transition: RUNNING -> PAUSED
        if (prev_state == XDAP_VM_RUNNING && ctrl->vm_state == XDAP_VM_PAUSED) {
            // Send stopped event
            xdap_send_stopped_event(ctrl, stop_reason_str(ctrl->stop_reason),
                                    ctrl->stopped_coro_id);
        }
        prev_state = ctrl->vm_state;

        // Execute VM if running
        if (ctrl->vm_state == XDAP_VM_RUNNING && ctrl->configured && ctrl->isolate) {
            if (!ctrl->program_launched) {
                // First launch: compile and execute program
                ctrl->program_launched = true;
                XrProto *proto = NULL;
                int result = xray_isolate_dofile_debug(ctrl->isolate, ctrl->program_path, (void**)&proto);
                ctrl->debug_proto = proto;

                if (ctrl->vm_state == XDAP_VM_PAUSED) {
                    // Stopped at breakpoint/step during initial execution
                    xdap_send_stopped_event(ctrl, stop_reason_str(ctrl->stop_reason),
                                            ctrl->stopped_coro_id);
                } else {
                    // Program terminated normally
                    if (proto) {
                        xr_free_code(ctrl->isolate, proto);
                        ctrl->debug_proto = NULL;
                    }
                    ctrl->vm_state = XDAP_VM_TERMINATED;
                    xdap_send_terminated_event(ctrl);
                    xdap_send_exited_event(ctrl, result);
                }
            } else {
                // Resume after pause: continue execution from where we stopped
                XdapResumeResult resume = xr_debug_resume_execution(ctrl->isolate);

                switch (resume) {
                    case XDAP_RESUME_STOPPED:
                        // Stopped again at breakpoint/step
                        xdap_send_stopped_event(ctrl, stop_reason_str(ctrl->stop_reason),
                                                ctrl->stopped_coro_id);
                        break;
                    case XDAP_RESUME_TERMINATED:
                    case XDAP_RESUME_ERROR:
                        // Program ended or runtime error
                        if (ctrl->debug_proto) {
                            xr_free_code(ctrl->isolate, ctrl->debug_proto);
                            ctrl->debug_proto = NULL;
                        }
                        ctrl->vm_state = XDAP_VM_TERMINATED;
                        xdap_send_terminated_event(ctrl);
                        xdap_send_exited_event(ctrl, resume == XDAP_RESUME_ERROR ? 1 : 0);
                        break;
                }
            }
            // Update prev_state after execution so the transition detection
            // at the top of the loop doesn't re-send the stopped event.
            prev_state = ctrl->vm_state;
        }
    }

    return 0;
}
