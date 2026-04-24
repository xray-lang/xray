/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_debug.c - Debug hook implementation for VM integration
 */

#include "xdap_controller.h"
#include "xdap_protocol.h"
#include "xdap_debug.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/xisolate_internal.h"
#include "../../runtime/xexec_state.h"
#include "../../base/xhash.h"
#include "../../runtime/xray_debug_hooks.h"
#include "../../runtime/xexec_frame.h"
#include "../../runtime/value/xvalue.h"
#include "../../runtime/value/xchunk.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/object/xarray.h"
#include "../../runtime/object/xmap.h"
#include "../../runtime/object/xjson.h"
#include "../../runtime/object/xstringbuilder.h"
#include "../../coro/xcoroutine.h"
#include "../../module/xmodule.h"
#include "../../runtime/class/xinstance.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../frontend/parser/xparse.h"
#include "../../frontend/parser/xast.h"
#include "../../vm/xdebug.h"
#include "../../coro/xworker.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../../base/xmalloc.h"

// Forward declaration (public, also used by xdap_eval.c)
XrCoroutine *xr_debug_get_coro(XrayIsolate *isolate);

// ============================================================================
// Internal Helpers
// ============================================================================

bool xr_debug_get_frame_ctx_ex(XrayIsolate *isolate, XrDebugFrameCtx *out) {
    XrCoroutine *coro = xr_debug_get_coro(isolate);
    if (coro) {
        out->frames = coro->vm_ctx.frames;
        out->stack = coro->vm_ctx.stack;
        out->frame_count = coro->vm_ctx.frame_count;
    } else {
        XrVMState *vm = xr_isolate_get_vm_state(isolate);
        out->frames = vm->frames;
        out->stack = vm->stack;
        out->frame_count = vm->frame_count;
    }
    return out->frame_count > 0;
}


// Evaluate condition string and return truthiness
// Used by both line breakpoints and function breakpoints
bool xr_debug_eval_condition_truthy(XrayIsolate *isolate, const char *condition) {
    if (!condition || condition[0] == '\0') return true;
    char *result = xr_debug_evaluate(isolate, condition, 0);
    bool is_truthy = result && (
        strcmp(result, "true") == 0 ||
        strcmp(result, "1") == 0 ||
        (result[0] != '\0' &&
         strcmp(result, "false") != 0 &&
         strcmp(result, "0") != 0 &&
         strcmp(result, "null") != 0 &&
         strncmp(result, "<undefined:", 11) != 0 &&
         strncmp(result, "<error:", 7) != 0)
    );
    xr_free(result);
    return is_truthy;
}

// ============================================================================
// VM Debug Hook Callbacks (new unified interface)
// ============================================================================

// Record stop position and signal the controller event loop.
// stop_reason tells the controller why we stopped (entry/breakpoint/step/pause).
static void hook_record_stop(XrayIsolate *isolate, XrDebugState *dbg,
                              const char *path, int line,
                              XrClosure *closure, int frame_depth,
                              XdapStopReason stop_reason) {
    xr_free(dbg->last_path);
    dbg->last_path = path ? xr_strdup(path) : NULL;
    dbg->last_line = line;
    dbg->step_depth = frame_depth;
    dbg->current_action = XR_DBG_ACTION_BREAK;

    xr_free(dbg->last_func_name);
    dbg->last_func_name = NULL;
    if (closure && closure->proto && closure->proto->name) {
        dbg->last_func_name = xr_strdup(XR_STRING_CHARS(closure->proto->name));
    }

    // Wire up the controller so the event loop sees PAUSED
    XdapController *ctrl = (XdapController *)xray_isolate_get_userdata(isolate);
    if (ctrl) {
        ctrl->vm_state = XDAP_VM_PAUSED;
        ctrl->stop_reason = stop_reason;
        ctrl->stopped_coro_id = 1;  // main thread
        XrCoroutine *coro = xr_debug_get_coro(isolate);
        ctrl->stopped_coro = coro;
    }
}

// Unified on_line hook — owns ALL debug decision logic.
// Called by VM at each line-change safe point.
static XrDebugAction hook_on_line(XrayIsolate *isolate, const char *path,
                                   int line, XrClosure *closure,
                                   XrBcCallFrame *frame, int frame_depth) {
    (void)frame;
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return XR_DBG_ACTION_CONTINUE;

    bool line_changed = (line != dbg->last_line);
    XrDebugAction action = dbg->current_action;

    // 1. Pause check (highest priority) — set by controller via atomic flag
    XdapController *ctrl = (XdapController *)xray_isolate_get_userdata(isolate);
    if (ctrl && atomic_load(&ctrl->cmd_pending)) {
        int cmd = atomic_load(&ctrl->pending_cmd);
        if (cmd == XDAP_CMD_PAUSE) {
            atomic_store(&ctrl->cmd_pending, false);
            hook_record_stop(isolate, dbg, path, line, closure, frame_depth, XDAP_STOP_PAUSE);
            return XR_DBG_ACTION_BREAK;
        }
    }

    // 2. Breakpoint check (only on line change)
    if (line_changed && path) {
        char *log_msg = NULL;
        int bp_result = xr_debug_check_breakpoint_ex(isolate, path, line, &log_msg);
        if (bp_result == 2 && log_msg) {
            // Logpoint: send output event, don't break
            if (ctrl) {
                xdap_send_output_event(ctrl, "console", log_msg);
            }
            xr_free(log_msg);
            // Fall through — don't return, check stepping too
        } else if (bp_result == 1) {
            xr_free(log_msg);
            hook_record_stop(isolate, dbg, path, line, closure, frame_depth, XDAP_STOP_BREAKPOINT);
            return XR_DBG_ACTION_BREAK;
        } else {
            xr_free(log_msg);
        }
    }

    // 3. Function breakpoint check (only on function entry — first line of proto)
    if (line_changed && closure && closure->proto && closure->proto->name) {
        int first_line = PROTO_LINE_COUNT(closure->proto) > 0
            ? PROTO_LINE(closure->proto, 0) : 0;
        if (line == first_line) {
            const char *func_name = XR_STRING_CHARS(closure->proto->name);
            if (xr_debug_check_function_breakpoint(isolate, func_name)) {
                hook_record_stop(isolate, dbg, path, line, closure, frame_depth,
                                 XDAP_STOP_BREAKPOINT);
                return XR_DBG_ACTION_BREAK;
            }
        }
    }

    // 4. Stepping logic
    bool should_break = false;
    if (action == XR_DBG_ACTION_BREAK && line_changed) {
        should_break = true;
    } else if (action == XR_DBG_ACTION_STEP_IN) {
        if (line_changed || frame_depth > dbg->step_depth) {
            should_break = true;
        }
    } else if (action == XR_DBG_ACTION_STEP_OVER) {
        if (frame_depth < dbg->step_depth) {
            should_break = true;
        } else if (line_changed && frame_depth <= dbg->step_depth) {
            should_break = true;
        }
    } else if (action == XR_DBG_ACTION_STEP_OUT) {
        if (frame_depth < dbg->step_depth) {
            should_break = true;
        }
    }

    if (should_break) {
        hook_record_stop(isolate, dbg, path, line, closure, frame_depth, XDAP_STOP_STEP);
        return XR_DBG_ACTION_BREAK;
    }

    return XR_DBG_ACTION_CONTINUE;
}

// Exception hook — called by VM/runtime when exception is thrown
static XrDebugAction hook_on_exception(XrayIsolate *isolate, const char *message,
                                        bool is_uncaught) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return XR_DBG_ACTION_CONTINUE;

    if ((is_uncaught && dbg->break_on_uncaught) ||
        (!is_uncaught && dbg->break_on_caught)) {
        xr_free(dbg->exception_message);
        dbg->exception_message = message ? xr_strdup(message) : NULL;
        dbg->exception_is_uncaught = is_uncaught;
        dbg->current_action = XR_DBG_ACTION_BREAK;

        // Signal controller event loop
        XdapController *ctrl = (XdapController *)xray_isolate_get_userdata(isolate);
        if (ctrl) {
            ctrl->vm_state = XDAP_VM_PAUSED;
            ctrl->stop_reason = XDAP_STOP_EXCEPTION;
            ctrl->stopped_coro_id = 1;
        }
        return XR_DBG_ACTION_BREAK;
    }
    return XR_DBG_ACTION_CONTINUE;
}

static bool hook_is_enabled(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    return dbg && dbg->enabled;
}

// Static hooks structure (stateless — per-isolate state via debug_state)
static XrDebugHooks g_debug_hooks = {
    .on_line = hook_on_line,
    .on_exception = hook_on_exception,
    .is_enabled = hook_is_enabled
};

// ============================================================================
// Value Helpers
// ============================================================================

const char* xr_value_type_name(XrValue val) {
    if (XR_IS_INT(val)) return TYPE_NAME_INT;
    if (XR_IS_FLOAT(val)) return TYPE_NAME_FLOAT;
    if (XR_IS_BOOL(val)) return TYPE_NAME_BOOL;
    if (XR_IS_NULL(val)) return TYPE_NAME_NULL;
    if (XR_IS_STRING(val)) return TYPE_NAME_STRING;
    if (XR_IS_ARRAY(val)) return TYPE_NAME_ARRAY;
    if (XR_IS_MAP(val)) return TYPE_NAME_MAP;
    if (XR_IS_SET(val)) return TYPE_NAME_SET;
    if (XR_IS_PTR(val)) {
        XrGCHeader *hdr = XR_TO_PTR(val);
        if (hdr->type == XR_TFUNCTION) return TYPE_NAME_FUNCTION;
        if (hdr->type == XR_TCFUNCTION) return TYPE_NAME_CFUNCTION;
    }
    return TYPE_NAME_OBJECT;
}

char* xr_value_to_debug_string(XrayIsolate *isolate, XrValue val) {
    char buf[512];

    if (XR_IS_INT(val)) {
        snprintf(buf, sizeof(buf), "%lld", (long long)XR_TO_INT(val));
    } else if (XR_IS_FLOAT(val)) {
        snprintf(buf, sizeof(buf), "%g", XR_TO_FLOAT(val));
    } else if (XR_IS_BOOL(val)) {
        snprintf(buf, sizeof(buf), "%s", XR_TO_BOOL(val) ? "true" : "false");
    } else if (XR_IS_NULL(val)) {
        snprintf(buf, sizeof(buf), "null");
    } else if (XR_IS_STRING(val)) {
        XrString *s = XR_TO_STRING(val);
        int len = s->length > 50 ? 50 : s->length;
        snprintf(buf, sizeof(buf), "\"%.*s%s\"", len, XR_STRING_CHARS(s),
                 s->length > 50 ? "..." : "");
    } else if (XR_IS_ARRAY(val)) {
        XrArray *arr = XR_TO_ARRAY(val);
        int size = xr_array_size(arr);
        snprintf(buf, sizeof(buf), "Array(%d) @%p", size, (void*)arr);
    } else if (XR_IS_MAP(val)) {
        XrMap *map = XR_TO_MAP(val);
        snprintf(buf, sizeof(buf), "Map(%u) @%p", map->count, (void*)map);
    } else if (XR_IS_SET(val)) {
        void *ptr = XR_TO_PTR(val);
        snprintf(buf, sizeof(buf), "Set{...} @%p", ptr);
    } else if (XR_IS_JSON(val)) {
        XrJson *json = xr_value_to_json(val);
        int count = xr_json_field_count(isolate, json);
        snprintf(buf, sizeof(buf), "Object(%d) @%p", count, (void*)json);
    } else if (XR_IS_PTR(val)) {
        XrGCHeader *hdr = XR_TO_PTR(val);
        switch (hdr->type) {
        case XR_TFUNCTION: {
            XrClosure *cl = (XrClosure *)hdr;
            const char *name = cl->proto && cl->proto->name ? XR_STRING_CHARS(cl->proto->name) : "anonymous";
            snprintf(buf, sizeof(buf), "function %s()", name);
            break;
        }
        case XR_TCFUNCTION:
            snprintf(buf, sizeof(buf), "<native function>");
            break;
        case XR_TINSTANCE: {
            XrInstance *inst = (XrInstance *)hdr;
            const char *class_name = inst->klass && inst->klass->name ? inst->klass->name : "Instance";
            snprintf(buf, sizeof(buf), "%s {...} @%p", class_name, (void*)hdr);
            break;
        }
        case XR_TCLASS: {
            XrClass *klass = (XrClass *)hdr;
            snprintf(buf, sizeof(buf), "class %s @%p", klass->name ? klass->name : "<anonymous>", (void*)hdr);
            break;
        }
        case XR_TCOROUTINE:
            snprintf(buf, sizeof(buf), "<coroutine> @%p", (void*)hdr);
            break;
        case XR_TCHANNEL:
            snprintf(buf, sizeof(buf), "<channel> @%p", (void*)hdr);
            break;
        case XR_TARRAY_SLICE: {
            XrArray *slice = (XrArray *)hdr;
            snprintf(buf, sizeof(buf), "Slice[%d] @%p", slice->length, (void*)hdr);
            break;
        }
        case XR_TREGEX:
            snprintf(buf, sizeof(buf), "<regex> @%p", (void*)hdr);
            break;
        case XR_TDATETIME:
            snprintf(buf, sizeof(buf), "<datetime> @%p", (void*)hdr);
            break;
        case XR_TSTRINGBUILDER: {
            XrStringBuilder *sb = (XrStringBuilder *)hdr;
            size_t len = xr_stringbuilder_length(sb);
            if (len <= 50) {
                XrString *s = xr_stringbuilder_to_string(sb);
                if (s) {
                    snprintf(buf, sizeof(buf), "StringBuilder(%zu) \"%.*s\"",
                             len, (int)len, XR_STRING_CHARS(s));
                } else {
                    snprintf(buf, sizeof(buf), "StringBuilder(%zu)", len);
                }
            } else {
                snprintf(buf, sizeof(buf), "StringBuilder(%zu)", len);
            }
            break;
        }
        case XR_TMODULE: {
            XrModule *mod = (XrModule *)hdr;
            snprintf(buf, sizeof(buf), "<module %s>", mod->name ? mod->name : "?");
            break;
        }
        case XR_TITERATOR:
            snprintf(buf, sizeof(buf), "<iterator> @%p", (void*)hdr);
            break;
        case XR_TERROR:
            snprintf(buf, sizeof(buf), "<error> @%p", (void*)hdr);
            break;
        case XR_TEXCEPTION:
            snprintf(buf, sizeof(buf), "<exception> @%p", (void*)hdr);
            break;
        case XR_TENUM_TYPE:
            snprintf(buf, sizeof(buf), "<enum type> @%p", (void*)hdr);
            break;
        case XR_TENUM_VALUE:
            snprintf(buf, sizeof(buf), "<enum value> @%p", (void*)hdr);
            break;
        case XR_TBOUND_METHOD:
            snprintf(buf, sizeof(buf), "<bound method> @%p", (void*)hdr);
            break;
        case XR_TBIGINT:
            snprintf(buf, sizeof(buf), "<bigint> @%p", (void*)hdr);
            break;
        case XR_TCOROPOOL:
            snprintf(buf, sizeof(buf), "<coro pool> @%p", (void*)hdr);
            break;
        case XR_TSHAPE:
            snprintf(buf, sizeof(buf), "<shape> @%p", (void*)hdr);
            break;
        default:
            snprintf(buf, sizeof(buf), "<object:%d> @%p", hdr->type, (void*)hdr);
            break;
        }
    } else {
        snprintf(buf, sizeof(buf), "<unknown>");
    }

    return xr_strdup(buf);
}

// ============================================================================
// Debug State Management
// ============================================================================

void xr_debug_init(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_calloc(1, sizeof(XrDebugState));
    if (!dbg) return;

    dbg->next_bp_id = 1;
    dbg->next_func_bp_id = XDAP_FUNC_BP_ID_BASE;
    dbg->next_var_ref_id = XDAP_VAR_REF_ID_BASE;
    dbg->enabled = false;
    dbg->current_action = XR_DBG_ACTION_CONTINUE;
    xr_isolate_set_debug_state(isolate, dbg);

    // Register VM debug hooks
    xr_debug_register_hooks(isolate, &g_debug_hooks);
}

void xr_debug_free(XrayIsolate *isolate) {
    if (!xr_isolate_get_debug_state(isolate)) return;

    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);

    // Clear breakpoint hash table first
    xr_bp_hash_clear(&dbg->bp_hash);

    // Free breakpoints
    struct XrBreakpoint *bp = dbg->breakpoints;
    while (bp) {
        struct XrBreakpoint *next = bp->next;
        xr_bp_free_fields(bp);
        xr_free(bp);
        bp = next;
    }

    // Free function breakpoints
    XrFuncBreakpoint *fbp = dbg->func_breakpoints;
    while (fbp) {
        XrFuncBreakpoint *next = fbp->next;
        xr_free(fbp->func_name);
        xr_free(fbp->condition);
        xr_free(fbp);
        fbp = next;
    }

    // Free watches
    struct XrWatch *w = dbg->watches;
    while (w) {
        struct XrWatch *next = w->next;
        xr_free(w->expression);
        xr_free(w);
        w = next;
    }

    // Free exception message
    xr_free(dbg->exception_message);

    // Free last stop location
    xr_free(dbg->last_path);
    xr_free(dbg->last_func_name);

    // Free variable references
    xr_free(dbg->var_refs);
    dbg->var_refs = NULL;

    xr_free(dbg);
    xr_isolate_set_debug_state(isolate, NULL);
}

// ============================================================================
// Hook Management
// ============================================================================

void xr_debug_enable(XrayIsolate *isolate, bool enable) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;

    dbg->enabled = enable;
}

// ============================================================================
// Execution Control
// ============================================================================

void xr_debug_continue(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    dbg->current_action = XR_DBG_ACTION_CONTINUE;
}

void xr_debug_step_in(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    dbg->current_action = XR_DBG_ACTION_STEP_IN;
    XrCoroutine *coro = xr_debug_get_coro(isolate);
    dbg->step_depth = coro ? coro->vm_ctx.frame_count : xr_isolate_get_vm_state(isolate)->frame_count;
}

void xr_debug_step_out(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    dbg->current_action = XR_DBG_ACTION_STEP_OUT;
    XrCoroutine *coro = xr_debug_get_coro(isolate);
    dbg->step_depth = coro ? coro->vm_ctx.frame_count : xr_isolate_get_vm_state(isolate)->frame_count;
}

void xr_debug_step_over(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    dbg->current_action = XR_DBG_ACTION_STEP_OVER;
    XrCoroutine *coro = xr_debug_get_coro(isolate);
    dbg->step_depth = coro ? coro->vm_ctx.frame_count : xr_isolate_get_vm_state(isolate)->frame_count;
}

// Resume VM execution after a debug break
// Returns true if execution continued (stopped at breakpoint), false if program ended
bool xr_debug_resume_execution(XrayIsolate *isolate) {
    if (!isolate) return false;

    XrCoroutine *coro = xr_debug_get_coro(isolate);
    if (!coro) return false;

    // Use the public API from xworker
    int result = xr_debug_resume_vm(isolate, coro);

    // result: 0 = stopped at breakpoint, 1 = program ended, -1 = error
    return (result == 0);
}

// ============================================================================
// Stack Inspection
// ============================================================================

// Helper to get main coroutine (where debug state is)
XrCoroutine *xr_debug_get_coro(XrayIsolate *isolate) {
    return xr_isolate_get_main_coro(isolate);
}

int xr_debug_get_stack_depth(XrayIsolate *isolate) {
    XrDebugFrameCtx fctx;
    xr_debug_get_frame_ctx_ex(isolate, &fctx);
    return fctx.frame_count;
}

bool xr_debug_get_frame_info(XrayIsolate *isolate, int frame_idx,
                              const char **out_func_name,
                              const char **out_source,
                              int *out_line) {
    XrDebugFrameCtx fctx;
    xr_debug_get_frame_ctx_ex(isolate, &fctx);
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);

    if (frame_idx < 0 || frame_idx >= fctx.frame_count) {
        return false;
    }

    // Frames are 0=bottom, frame_count-1=top
    // For debugger, 0=top is more intuitive
    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];

    // For top frame (frame_idx=0), use saved debug state info
    // This is more reliable as proto->source_file may be NULL for module-level code
    if (frame_idx == 0 && dbg && dbg->last_path) {
        if (out_func_name) {
            if (frame->closure && frame->closure->proto && frame->closure->proto->name) {
                *out_func_name = XR_STRING_CHARS(frame->closure->proto->name);
            } else {
                *out_func_name = "<module>";
            }
        }
        if (out_source) {
            *out_source = dbg->last_path;
        }
        if (out_line) {
            *out_line = dbg->last_line;
        }
        return true;
    }

    if (!frame->closure || !frame->closure->proto) {
        return false;
    }

    XrProto *proto = frame->closure->proto;

    if (out_func_name) {
        *out_func_name = proto->name ? XR_STRING_CHARS(proto->name) : "<anonymous>";
    }

    if (out_source) {
        *out_source = proto->source_file ? proto->source_file : "<unknown>";
    }

    if (out_line) {
        // Calculate line from PC
        int pc_offset = (int)(frame->pc - PROTO_CODE_BASE(proto));
        int code_count = PROTO_CODE_COUNT(proto);
        int line_count = PROTO_LINE_COUNT(proto);
        if (pc_offset >= 0 && pc_offset < code_count && pc_offset < line_count) {
            *out_line = PROTO_LINE(proto, pc_offset);
        } else {
            *out_line = 1;
        }
    }

    return true;
}

// NOTE: Variable inspection, breakpoints, watches, coroutine debugging,
// disassembly, async stack, set variable, function breakpoints, and hot reload
// have been moved to xdap_breakpoints.c and xdap_variables.c
