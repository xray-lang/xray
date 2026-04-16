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
 *   Minimal callback interface for debugger integration.
 *   VM calls these hooks, debugger (DAP) registers implementations.
 *   When no debugger is attached, hooks are NULL - zero overhead.
 */

#ifndef XRAY_DEBUG_HOOKS_H
#define XRAY_DEBUG_HOOKS_H

#include <stdbool.h>

#include "../base/xforward_decl.h"
#include "../base/xdefs.h"

/*
 * Debug action returned by hooks
 */
typedef enum {
    XR_DBG_ACTION_CONTINUE = 0,
    XR_DBG_ACTION_BREAK,
    XR_DBG_ACTION_STEP_IN,
    XR_DBG_ACTION_STEP_OUT,
    XR_DBG_ACTION_STEP_OVER
} XrDebugAction;

/*
 * Debug hooks - function pointers registered by debugger
 */
typedef struct XrDebugHooks {
    // Check if breakpoint exists at path:line
    bool (*check_breakpoint)(XrayIsolate *isolate, const char *path, int line);
    
    // Called when breakpoint is hit
    void (*on_breakpoint_hit)(XrayIsolate *isolate, const char *path, int line,
                               XrClosure *closure, XrBcCallFrame *frame);
    
    // Get current debug action (for stepping)
    XrDebugAction (*get_action)(XrayIsolate *isolate);
    
    // Get step depth (for step-over/step-out)
    int (*get_step_depth)(XrayIsolate *isolate);
    
    // Set last line (for step-over same line skip)
    void (*set_last_line)(XrayIsolate *isolate, int line);
    
    // Get last line
    int (*get_last_line)(XrayIsolate *isolate);
    
    // Check if debug is enabled
    bool (*is_enabled)(XrayIsolate *isolate);
} XrDebugHooks;

/*
 * Register debug hooks (called by DAP module on init)
 */
XR_FUNC void xr_debug_register_hooks(XrayIsolate *isolate, XrDebugHooks *hooks);

/*
 * Get registered hooks (used by VM)
 */
XR_FUNC XrDebugHooks *xr_debug_get_hooks(XrayIsolate *isolate);

#endif // XRAY_DEBUG_HOOKS_H
