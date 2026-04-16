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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../vm/xvm_internal.h"
#include "xray.h"

// ============================================================================
// JSON Helpers
// ============================================================================

static inline XrJsonValue *json_get(XrJsonValue *obj, const char *key) {
    return xlsp_json_get(obj, key);
}

static inline double json_number(XrJsonValue *v) {
    return v && v->type == XR_JSON_NUMBER ? v->as.number : 0;
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
    XrJsonValue *resp = xlsp_json_new_object();
    xlsp_json_object_set(resp, "seq", xlsp_json_new_number(ctrl->seq++));
    xlsp_json_object_set(resp, "type", xlsp_json_new_string("response"));
    xlsp_json_object_set(resp, "request_seq", xlsp_json_new_number(request_seq));
    xlsp_json_object_set(resp, "success", xlsp_json_new_bool(success));
    xlsp_json_object_set(resp, "command", xlsp_json_new_string(command));
    
    if (body) {
        xlsp_json_object_set(resp, "body", body);
    }
    if (error_message) {
        xlsp_json_object_set(resp, "message", xlsp_json_new_string(error_message));
    }
    
    size_t len = 0;
    char *json = xlsp_json_stringify(resp, &len);
    xdap_transport_write(ctrl->transport, json, len);
    free(json);
    xlsp_json_free(resp);
}

void xdap_send_event(XdapController *ctrl, const char *event, XrJsonValue *body) {
    XrJsonValue *evt = xlsp_json_new_object();
    xlsp_json_object_set(evt, "seq", xlsp_json_new_number(ctrl->seq++));
    xlsp_json_object_set(evt, "type", xlsp_json_new_string("event"));
    xlsp_json_object_set(evt, "event", xlsp_json_new_string(event));
    
    if (body) {
        xlsp_json_object_set(evt, "body", body);
    }
    
    size_t len = 0;
    char *json = xlsp_json_stringify(evt, &len);
    xdap_transport_write(ctrl->transport, json, len);
    free(json);
    xlsp_json_free(evt);
}

// ============================================================================
// Common Events
// ============================================================================

void xdap_send_initialized_event(XdapController *ctrl) {
    xdap_send_event(ctrl, "initialized", NULL);
}

void xdap_send_stopped_event(XdapController *ctrl, const char *reason, int thread_id) {
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "reason", xlsp_json_new_string(reason));
    xlsp_json_object_set(body, "threadId", xlsp_json_new_number(thread_id));
    xlsp_json_object_set(body, "allThreadsStopped", xlsp_json_new_bool(true));
    xdap_send_event(ctrl, "stopped", body);
}

void xdap_send_terminated_event(XdapController *ctrl) {
    xdap_send_event(ctrl, "terminated", NULL);
}

void xdap_send_exited_event(XdapController *ctrl, int exit_code) {
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "exitCode", xlsp_json_new_number(exit_code));
    xdap_send_event(ctrl, "exited", body);
}

void xdap_send_output_event(XdapController *ctrl, const char *category,
                             const char *output) {
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "category", xlsp_json_new_string(category));
    xlsp_json_object_set(body, "output", xlsp_json_new_string(output));
    xdap_send_event(ctrl, "output", body);
}

// ============================================================================
// Request Handlers
// ============================================================================

static void handle_initialize(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;
    
    XrJsonValue *body = xlsp_json_new_object();
    
    // Capabilities
    xlsp_json_object_set(body, "supportsConfigurationDoneRequest", 
                          xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsConditionalBreakpoints", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsHitConditionalBreakpoints", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsLogPoints", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsFunctionBreakpoints", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsTerminateRequest", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsRestartRequest", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsSetVariable", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsEvaluateForHovers", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsDisassembleRequest", xlsp_json_new_bool(true));
    xlsp_json_object_set(body, "supportsSteppingGranularity", xlsp_json_new_bool(true));
    
    // Exception breakpoint filters
    XrJsonValue *filters = xlsp_json_new_array();
    
    XrJsonValue *uncaught = xlsp_json_new_object();
    xlsp_json_object_set(uncaught, "filter", xlsp_json_new_string("uncaught"));
    xlsp_json_object_set(uncaught, "label", xlsp_json_new_string("Uncaught Exceptions"));
    xlsp_json_object_set(uncaught, "default", xlsp_json_new_bool(true));
    xlsp_json_array_push(filters, uncaught);
    
    XrJsonValue *caught = xlsp_json_new_object();
    xlsp_json_object_set(caught, "filter", xlsp_json_new_string("caught"));
    xlsp_json_object_set(caught, "label", xlsp_json_new_string("Caught Exceptions"));
    xlsp_json_object_set(caught, "default", xlsp_json_new_bool(false));
    xlsp_json_array_push(filters, caught);
    
    xlsp_json_object_set(body, "exceptionBreakpointFilters", filters);
    
    xdap_send_response(ctrl, seq, "initialize", true, body, NULL);
    ctrl->initialized = true;
    
    xdap_send_initialized_event(ctrl);
}

static void handle_launch(XdapController *ctrl, int seq, XrJsonValue *args) {
    XrJsonValue *program_val = json_get(args, "program");
    const char *program = json_string(program_val);
    
    if (!program) {
        xdap_send_response(ctrl, seq, "launch", false, NULL, "Missing 'program' argument");
        return;
    }
    
    // Get arguments
    XrJsonValue *args_array = json_get(args, "args");
    char **argv = NULL;
    int argc = 0;
    
    if (args_array && xlsp_json_is_array(args_array)) {
        argc = xlsp_json_array_len(args_array);
        if (argc > 0) {
            argv = xr_calloc((size_t)argc, sizeof(char *));
            if (!argv) {
                xdap_send_response(ctrl, seq, "launch", false, NULL, "Out of memory");
                return;
            }
            for (int i = 0; i < argc; i++) {
                XrJsonValue *arg = xlsp_json_array_get(args_array, i);
                if (arg && arg->type == XR_JSON_STRING) {
                    argv[i] = strdup(arg->as.string);
                    if (!argv[i]) {
                        // Cleanup on failure
                        for (int j = 0; j < i; j++) free(argv[j]);
                        free(argv);
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
        for (int i = 0; i < argc; i++) free(argv[i]);
        free(argv);
        return;
    }
    
    // Cleanup (controller made copies)
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    
    xdap_send_response(ctrl, seq, "launch", true, NULL, NULL);
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
    
    XrJsonValue *result_bps = xlsp_json_new_array();
    
    int bp_count = breakpoints ? xlsp_json_array_len(breakpoints) : 0;
    for (int i = 0; i < bp_count; i++) {
        XrJsonValue *bp = xlsp_json_array_get(breakpoints, i);
        int line = (int)json_number(json_get(bp, "line"));
        const char *condition = json_string(json_get(bp, "condition"));
        const char *log_message = json_string(json_get(bp, "logMessage"));
        const char *hit_condition = json_string(json_get(bp, "hitCondition"));
        
        // Directly use debug API
        int id = xr_debug_add_breakpoint_ex(ctrl->isolate, path, line, 
                                             condition, log_message, hit_condition);
        
        XrJsonValue *result_bp = xlsp_json_new_object();
        xlsp_json_object_set(result_bp, "id", xlsp_json_new_number(id));
        xlsp_json_object_set(result_bp, "verified", xlsp_json_new_bool(id > 0));
        xlsp_json_object_set(result_bp, "line", xlsp_json_new_number(line));
        xlsp_json_array_push(result_bps, result_bp);
    }
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "breakpoints", result_bps);
    
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
    
    XrJsonValue *result_bps = xlsp_json_new_array();
    
    int bp_count = breakpoints ? xlsp_json_array_len(breakpoints) : 0;
    for (int i = 0; i < bp_count; i++) {
        XrJsonValue *bp = xlsp_json_array_get(breakpoints, i);
        const char *name = json_string(json_get(bp, "name"));
        const char *condition = json_string(json_get(bp, "condition"));
        
        // Directly use debug API
        int id = xr_debug_add_function_breakpoint(ctrl->isolate, name, condition);
        
        XrJsonValue *result_bp = xlsp_json_new_object();
        xlsp_json_object_set(result_bp, "id", xlsp_json_new_number(id));
        xlsp_json_object_set(result_bp, "verified", xlsp_json_new_bool(id > 0));
        xlsp_json_array_push(result_bps, result_bp);
    }
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "breakpoints", result_bps);
    
    xdap_send_response(ctrl, seq, "setFunctionBreakpoints", true, body, NULL);
}

static void handle_set_exception_breakpoints(XdapController *ctrl, int seq, XrJsonValue *args) {
    XrJsonValue *filters = json_get(args, "filters");
    
    bool break_uncaught = false;
    bool break_caught = false;
    
    if (filters && xlsp_json_is_array(filters)) {
        int count = xlsp_json_array_len(filters);
        for (int i = 0; i < count; i++) {
            XrJsonValue *filter = xlsp_json_array_get(filters, i);
            const char *filter_str = json_string(filter);
            if (filter_str) {
                if (strcmp(filter_str, "uncaught") == 0) break_uncaught = true;
                if (strcmp(filter_str, "caught") == 0) break_caught = true;
                if (strcmp(filter_str, "all") == 0) {
                    break_uncaught = true;
                    break_caught = true;
                }
            }
        }
    }
    
    // Forward to debug state (single source of truth)
    if (ctrl->isolate) {
        xr_debug_set_exception_breakpoints(ctrl->isolate, break_uncaught, break_caught);
    }
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "breakpoints", xlsp_json_new_array());
    
    xdap_send_response(ctrl, seq, "setExceptionBreakpoints", true, body, NULL);
}

static void handle_threads(XdapController *ctrl, int seq, XrJsonValue *args) {
    (void)args;
    
    XrJsonValue *threads = xlsp_json_new_array();
    
    if (ctrl->isolate) {
        // Get all coroutines from the scheduler
        int coro_count = xr_debug_get_coro_count(ctrl->isolate);
        
        if (coro_count > 0) {
            for (int i = 0; i < coro_count; i++) {
                int coro_id;
                const char *coro_name;
                const char *coro_state;
                
                if (xr_debug_get_coro_info(ctrl->isolate, i, &coro_id, &coro_name, &coro_state)) {
                    XrJsonValue *thread = xlsp_json_new_object();
                    xlsp_json_object_set(thread, "id", xlsp_json_new_number(coro_id > 0 ? coro_id : 1));
                    
                    // Build thread name with state
                    char name_buf[128];
                    if (coro_name) {
                        snprintf(name_buf, sizeof(name_buf), "%s (%s)", coro_name, coro_state);
                    } else if (i == 0) {
                        snprintf(name_buf, sizeof(name_buf), "main (%s)", coro_state);
                    } else {
                        snprintf(name_buf, sizeof(name_buf), "coroutine-%d (%s)", coro_id, coro_state);
                    }
                    xlsp_json_object_set(thread, "name", xlsp_json_new_string(name_buf));
                    xlsp_json_array_push(threads, thread);
                }
            }
        } else {
            // No scheduler, just add main thread
            XrJsonValue *main_thread = xlsp_json_new_object();
            xlsp_json_object_set(main_thread, "id", xlsp_json_new_number(1));
            xlsp_json_object_set(main_thread, "name", xlsp_json_new_string("main"));
            xlsp_json_array_push(threads, main_thread);
        }
    } else {
        // No isolate, return empty (or default main thread)
        XrJsonValue *main_thread = xlsp_json_new_object();
        xlsp_json_object_set(main_thread, "id", xlsp_json_new_number(1));
        xlsp_json_object_set(main_thread, "name", xlsp_json_new_string("main"));
        xlsp_json_array_push(threads, main_thread);
    }
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "threads", threads);
    
    xdap_send_response(ctrl, seq, "threads", true, body, NULL);
}

static void handle_stack_trace(XdapController *ctrl, int seq, XrJsonValue *args) {
    int thread_id = (int)json_number(json_get(args, "threadId"));
    
    // Find the coroutine for this thread
    XrCoroutine *coro = xdap_find_coro(ctrl, thread_id);
    
    XrJsonValue *frames = NULL;
    
    if (coro && ctrl->vm_state == XDAP_VM_PAUSED) {
        int frame_id = 0;
        frames = xdap_inspect_stack_frames(ctrl, coro, &frame_id);
    }
    
    if (!frames) {
        frames = xlsp_json_new_array();
    }
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "stackFrames", frames);
    xlsp_json_object_set(body, "totalFrames", xlsp_json_new_number(xlsp_json_array_len(frames)));
    
    xdap_send_response(ctrl, seq, "stackTrace", true, body, NULL);
}

static void handle_scopes(XdapController *ctrl, int seq, XrJsonValue *args) {
    int frame_id = (int)json_number(json_get(args, "frameId"));
    
    if (!ctrl->isolate) {
        xdap_send_response(ctrl, seq, "scopes", false, NULL, "No active session");
        return;
    }
    
    XrJsonValue *scopes = xlsp_json_new_array();
    
    // Create locals scope reference (directly use debug API)
    int locals_ref = xr_debug_create_var_ref(ctrl->isolate, XDAP_REF_SCOPE_LOCALS,
                                              frame_id, XR_NULL_VAL);
    
    XrJsonValue *local = xlsp_json_new_object();
    xlsp_json_object_set(local, "name", xlsp_json_new_string("Locals"));
    xlsp_json_object_set(local, "variablesReference", xlsp_json_new_number(locals_ref));
    xlsp_json_object_set(local, "expensive", xlsp_json_new_bool(false));
    xlsp_json_array_push(scopes, local);
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "scopes", scopes);
    
    xdap_send_response(ctrl, seq, "scopes", true, body, NULL);
}

static void handle_variables(XdapController *ctrl, int seq, XrJsonValue *args) {
    int var_ref = (int)json_number(json_get(args, "variablesReference"));
    
    XrJsonValue *variables = xdap_inspect_variables(ctrl, var_ref);
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "variables", variables ? variables : xlsp_json_new_array());
    
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
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "allThreadsContinued", xlsp_json_new_bool(true));
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
    
    XrJsonValue *body = xlsp_json_new_object();
    
    if (expression && ctrl->isolate) {
        int var_ref = 0;
        char *result = xdap_inspect_evaluate_ex(ctrl, expression, frame_id, &var_ref);
        xlsp_json_object_set(body, "result", xlsp_json_new_string(result ? result : "<error>"));
        xlsp_json_object_set(body, "variablesReference", xlsp_json_new_number(var_ref));
        free(result);
    } else {
        xlsp_json_object_set(body, "result", xlsp_json_new_string("<no expression>"));
        xlsp_json_object_set(body, "variablesReference", xlsp_json_new_number(0));
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
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "value", xlsp_json_new_string(new_value));
    free(new_value);
    
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
    
    XrJsonValue *instructions = xlsp_json_new_array();
    for (int i = 0; i < count; i++) {
        XrJsonValue *instr = xlsp_json_new_object();
        
        char addr_buf[32];
        snprintf(addr_buf, sizeof(addr_buf), "0x%08x", instrs[i].offset);
        xlsp_json_object_set(instr, "address", xlsp_json_new_string(addr_buf));
        
        char *text = instrs[i].instruction ? instrs[i].instruction : "???";
        if (instrs[i].comment) {
            char combined[512];
            snprintf(combined, sizeof(combined), "%s  ; %s", text, instrs[i].comment);
            xlsp_json_object_set(instr, "instruction", xlsp_json_new_string(combined));
        } else {
            xlsp_json_object_set(instr, "instruction", xlsp_json_new_string(text));
        }
        
        if (instrs[i].line > 0) {
            xlsp_json_object_set(instr, "line", xlsp_json_new_number(instrs[i].line));
        }
        
        // Mark current instruction
        if (instrs[i].offset == current_pc) {
            xlsp_json_object_set(instr, "symbol", xlsp_json_new_string(">>>"));
        }
        
        xlsp_json_array_push(instructions, instr);
    }
    
    xr_debug_free_disasm(instrs, count);
    
    XrJsonValue *body = xlsp_json_new_object();
    xlsp_json_object_set(body, "instructions", instructions);
    
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
    (void)args;
    
    ctrl->vm_state = XDAP_VM_TERMINATED;
    xdap_send_response(ctrl, seq, "disconnect", true, NULL, NULL);
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
    XrJsonValue *msg = xlsp_json_parse(json, len);
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
    
    xlsp_json_free(msg);
    
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
                    free(msg);
                    break;  // Session ended
                }
                free(msg);
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
                bool still_running = xr_debug_resume_execution(ctrl->isolate);
                
                if (ctrl->vm_state == XDAP_VM_PAUSED) {
                    // Stopped again at breakpoint/step
                    xdap_send_stopped_event(ctrl, stop_reason_str(ctrl->stop_reason),
                                            ctrl->stopped_coro_id);
                } else if (!still_running) {
                    // Program terminated
                    if (ctrl->debug_proto) {
                        xr_free_code(ctrl->isolate, ctrl->debug_proto);
                        ctrl->debug_proto = NULL;
                    }
                    ctrl->vm_state = XDAP_VM_TERMINATED;
                    xdap_send_terminated_event(ctrl);
                    xdap_send_exited_event(ctrl, 0);
                }
            }
        }
    }
    
    return 0;
}
