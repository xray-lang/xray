/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_debug_hooks.h - Debug hook interface for VM
 *
 * KEY CONCEPT:
 *   Three-callback interface for debugger integration.
 *   VM calls on_line at each safe point (line change); the hook impl
 *   owns ALL decision logic: breakpoints, stepping, pause, logpoints,
 *   function breakpoints.  Returns BREAK or CONTINUE.
 *   VM calls on_exception when a runtime exception is thrown.
 *   When no debugger is attached, hooks pointer is NULL — zero overhead.
 */

#ifndef XRAY_DEBUG_HOOKS_H
#define XRAY_DEBUG_HOOKS_H

#include <stdbool.h>

#include "../base/xforward_decl.h"
#include "../base/xdefs.h"

/*
 * Debug action returned by hooks.
 * CONTINUE = keep running, BREAK = return XR_VM_DEBUG_BREAK to caller.
 * STEP_IN / STEP_OVER / STEP_OUT are internal to the hook impl and
 * are never returned to the VM — the hook converts them to BREAK or
 * CONTINUE before returning.
 */
typedef enum {
    XR_DBG_ACTION_CONTINUE = 0,
    XR_DBG_ACTION_BREAK,
    XR_DBG_ACTION_STEP_IN,
    XR_DBG_ACTION_STEP_OUT,
    XR_DBG_ACTION_STEP_OVER
} XrDebugAction;

/*
 * Debug hooks — registered by debugger (DAP), consumed by VM.
 *
 * Contract:
 *   - VM resolves (path, line) from the current PC.
 *   - VM calls on_line once per safe point when line > 0.
 *   - on_line returns BREAK → VM saves pc and returns XR_VM_DEBUG_BREAK.
 *   - on_line returns CONTINUE → VM keeps executing.
 *   - VM calls on_exception when a runtime exception is thrown.
 *   - is_enabled is a fast check; if false the VM skips the hook entirely.
 */
typedef struct XrDebugHooks {
    /* Called by VM at each line-change safe point.
     * Replaces the old check_breakpoint + on_breakpoint_hit + get_action +
     * get_step_depth + set_last_line + get_last_line — all in one call.
     * Hook impl decides: breakpoint? step? pause? logpoint? function bp? */
    XrDebugAction (*on_line)(XrayIsolate *isolate, const char *path,
                             int line, XrClosure *closure,
                             XrBcCallFrame *frame, int frame_depth);

    /* Called by VM/runtime when exception is thrown.
     * Returns BREAK to stop, CONTINUE to propagate normally. */
    XrDebugAction (*on_exception)(XrayIsolate *isolate, const char *message,
                                  bool is_uncaught);

    /* Quick check — allows VM to skip on_line entirely. */
    bool (*is_enabled)(XrayIsolate *isolate);
} XrDebugHooks;

/*
 * Register / retrieve debug hooks (called by DAP module on init).
 */
XR_FUNC void xr_debug_register_hooks(XrayIsolate *isolate, XrDebugHooks *hooks);
XR_FUNC XrDebugHooks *xr_debug_get_hooks(XrayIsolate *isolate);

#endif // XRAY_DEBUG_HOOKS_H
