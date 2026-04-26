/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_debug.h - Debug hook interface for VM integration
 *
 * KEY CONCEPT:
 *   Provides callback hooks for breakpoints, stepping, and variable inspection.
 *   VM calls these hooks at appropriate points during execution.
 */

#ifndef XDAP_DEBUG_H
#define XDAP_DEBUG_H

#include <stdbool.h>
#include <stdint.h>
#include "../../runtime/xray_debug_hooks.h"

// Forward declarations via xforward_decl.h
typedef struct XrProto XrProto;

// Value type
#include "../../runtime/value/xvalue.h"

// XrDebugAction is defined in xray_debug_hooks.h

// ============================================================================
// Resume Result (replaces bool still_running)
// ============================================================================

typedef enum {
    XDAP_RESUME_STOPPED,     // Debug break — send stopped event
    XDAP_RESUME_TERMINATED,  // Program ended normally
    XDAP_RESUME_ERROR,       // Runtime error
} XdapResumeResult;

// ============================================================================
// Variable Reference Types (for object expansion)
// ============================================================================

// ID space allocation to avoid collisions:
// - Breakpoint IDs: 1-999
// - Variable reference IDs: 1000-4999 (avoid collision with frameId which starts at 0)
// - Function breakpoint IDs: 5000+
#define XDAP_VAR_REF_ID_BASE 1000
#define XDAP_FUNC_BP_ID_BASE 5000

typedef enum {
    XDAP_REF_INVALID = 0,
    XDAP_REF_SCOPE_LOCALS,    // Local variables scope
    XDAP_REF_SCOPE_GLOBALS,   // Global variables scope
    XDAP_REF_SCOPE_UPVALUES,  // Closure upvalues scope
    XDAP_REF_ARRAY,           // Array elements
    XDAP_REF_MAP,             // Map key-value pairs
    XDAP_REF_OBJECT,          // Object/Json properties
    XDAP_REF_INSTANCE,        // Class instance fields
} XdapVarRefType;

typedef struct XrDebugVarRef {
    int id;  // Unique ID returned to DAP client
    XdapVarRefType type;
    int frame_idx;  // Associated stack frame (-1 for global)
    XrValue value;  // The value being referenced
} XrDebugVarRef;

// Variable info returned by expansion
typedef struct XdapVarInfo {
    char *name;
    char *value;
    char *type;
    int var_ref;        // >0 if expandable
    int named_count;    // Number of named children
    int indexed_count;  // Number of indexed children
} XdapVarInfo;

// Function breakpoint structure
typedef struct XrFuncBreakpoint {
    int id;
    char *func_name;
    char *condition;
    struct XrFuncBreakpoint *next;
} XrFuncBreakpoint;

// Breakpoint hash table for O(1) lookup
#define XR_BP_HASH_SIZE 64  // Power of 2 for fast modulo

typedef struct XrBpHashEntry {
    struct XrBreakpoint *bp;
    struct XrBpHashEntry *next;  // For collision chain
} XrBpHashEntry;

typedef struct XrBpHashTable {
    XrBpHashEntry *buckets[XR_BP_HASH_SIZE];
} XrBpHashTable;

typedef struct XrDebugState {
    // Execution control
    bool enabled;  // Debug mode enabled
    XrDebugAction current_action;
    int step_depth;  // Frame depth for step-over/step-out

    // Breakpoints (list + hash table for fast lookup)
    struct XrBreakpoint {
        int id;
        char *path;  // Source file path
        int line;    // Line number
        bool enabled;
        char *condition;            // Optional condition expression
        char *log_message;          // Log message for logpoints (NULL = normal breakpoint)
        int hit_count;              // Number of times this breakpoint was hit
        char *hit_condition;        // Hit count condition (e.g., ">5", "==10")
        struct XrBreakpoint *next;  // For linked list
    } *breakpoints;
    int next_bp_id;

    // Hash table for O(1) breakpoint lookup by (path, line)
    XrBpHashTable bp_hash;

    // Function breakpoints (per-isolate, not global)
    XrFuncBreakpoint *func_breakpoints;
    int next_func_bp_id;

    // Watch expressions
    struct XrWatch {
        int id;
        char *expression;
        struct XrWatch *next;
    } *watches;
    int next_watch_id;

    // Last stop location (for step-over, owned copy via strdup)
    char *last_path;
    int last_line;
    char *last_func_name;  // Function name at last stop (owned)

    // Exception breakpoints
    bool break_on_uncaught;  // Break on uncaught exceptions
    bool break_on_caught;    // Break on caught exceptions

    // Current exception info (when stopped on exception)
    char *exception_message;
    bool exception_is_uncaught;

    // Variable references for object expansion (dynamic array, O(1) lookup by ID)
    XrDebugVarRef *var_refs;  // Array indexed by (id - XDAP_VAR_REF_ID_BASE)
    int var_refs_count;       // Number of allocated refs
    int var_refs_capacity;    // Allocated capacity
    int next_var_ref_id;      // Next ID (starts at XDAP_VAR_REF_ID_BASE)

    // Async stack trace scratch buffers (per-isolate, avoids global state)
    const char *async_names[8];
    const char *async_files[8];
    int async_lines[8];
} XrDebugState;

// ============================================================================
// Debug API
// ============================================================================

// Frame context helper (shared between xdap_debug.c and xdap_variables.c)
typedef struct {
    XrBcCallFrame *frames;
    XrValue *stack;
    int frame_count;
} XrDebugFrameCtx;

// Initialize/cleanup debug state
XR_FUNC void xr_debug_init(XrayIsolate *isolate);
XR_FUNC void xr_debug_free(XrayIsolate *isolate);

// Hook management
XR_FUNC void xr_debug_enable(XrayIsolate *isolate, bool enable);

// Internal helpers (shared between split files)
XR_FUNC bool xr_debug_get_frame_ctx_ex(XrayIsolate *isolate, XrDebugFrameCtx *out);
XR_FUNC bool xr_debug_eval_condition_truthy(XrayIsolate *isolate, const char *condition);
XR_FUNC void xr_bp_hash_clear(XrBpHashTable *table);
XR_FUNC void xr_bp_free_fields(struct XrBreakpoint *bp);
XR_FUNC struct XrBreakpoint *xr_bp_hash_find(XrBpHashTable *table, const char *path, int line);

// Breakpoint management
XR_FUNC int xr_debug_add_breakpoint(XrayIsolate *isolate, const char *path, int line,
                                    const char *condition);
XR_FUNC int xr_debug_add_breakpoint_ex(XrayIsolate *isolate, const char *path, int line,
                                       const char *condition, const char *log_message,
                                       const char *hit_condition);
XR_FUNC bool xr_debug_remove_breakpoint(XrayIsolate *isolate, int id);
XR_FUNC void xr_debug_clear_breakpoints(XrayIsolate *isolate, const char *path);

// Returns: 0 = no breakpoint, 1 = should break, 2 = logpoint (message printed)
XR_FUNC int xr_debug_check_breakpoint_ex(XrayIsolate *isolate, const char *path, int line,
                                         char **out_log_message);
XR_FUNC bool xr_debug_check_breakpoint(XrayIsolate *isolate, const char *path, int line);

// Watch expression management
XR_FUNC int xr_debug_add_watch(XrayIsolate *isolate, const char *expression);
XR_FUNC bool xr_debug_remove_watch(XrayIsolate *isolate, int id);
XR_FUNC void xr_debug_clear_watches(XrayIsolate *isolate);
XR_FUNC int xr_debug_get_watch_count(XrayIsolate *isolate);
XR_FUNC bool xr_debug_get_watch(XrayIsolate *isolate, int idx, int *out_id, const char **out_expr);

// Execution control
XR_FUNC void xr_debug_continue(XrayIsolate *isolate);
XR_FUNC void xr_debug_step_in(XrayIsolate *isolate);
XR_FUNC void xr_debug_step_out(XrayIsolate *isolate);
XR_FUNC void xr_debug_step_over(XrayIsolate *isolate);
XR_FUNC XdapResumeResult xr_debug_resume_execution(XrayIsolate *isolate);

// Stack inspection
XR_FUNC int xr_debug_get_stack_depth(XrayIsolate *isolate);
XR_FUNC bool xr_debug_get_frame_info(XrayIsolate *isolate, int frame_idx,
                                     const char **out_func_name, const char **out_source,
                                     int *out_line);

// Variable inspection
XR_FUNC int xr_debug_get_local_count(XrayIsolate *isolate, int frame_idx);
XR_FUNC bool xr_debug_get_local(XrayIsolate *isolate, int frame_idx, int local_idx,
                                const char **out_name, char **out_value, char **out_type);

// Value formatting (shared with xdap_inspect and xdap_eval)
XR_FUNC const char *xr_value_type_name(XrValue val);
XR_FUNC char *xr_value_to_debug_string(XrayIsolate *isolate, XrValue val);

// Get the debug coroutine (main coro for single-threaded debug)
XR_FUNC XrCoroutine *xr_debug_get_coro(XrayIsolate *isolate);

// Expression evaluation
XR_FUNC char *xr_debug_evaluate(XrayIsolate *isolate, const char *expression, int frame_idx);

// Enhanced expression evaluation with expandable result support
// Returns result string, sets out_var_ref if result is expandable (>0)
XR_FUNC char *xr_debug_evaluate_ex(XrayIsolate *isolate, const char *expression, int frame_idx,
                                   int *out_var_ref);

// Exception breakpoints
XR_FUNC void xr_debug_set_exception_breakpoints(XrayIsolate *isolate, bool uncaught, bool caught);
XR_FUNC XrDebugAction xr_debug_on_exception(XrayIsolate *isolate, const char *message,
                                            bool is_uncaught);

// Coroutine debugging
XR_FUNC int xr_debug_get_coro_count(XrayIsolate *isolate);
XR_FUNC bool xr_debug_get_coro_info(XrayIsolate *isolate, int coro_idx, int *out_id,
                                    const char **out_name, const char **out_state);

// Hot reload
XR_FUNC bool xr_debug_hot_reload(XrayIsolate *isolate, const char *path);

// ============================================================================
// Disassembly API
// ============================================================================

// Disassembled instruction info
typedef struct XdapDisasmInstr {
    int offset;         // Instruction offset (address)
    int line;           // Source line number (-1 if unknown)
    char *instruction;  // Disassembled instruction text
    char *comment;      // Optional comment (constant value, etc.)
} XdapDisasmInstr;

// Get disassembly for current frame
// Returns array of instructions, caller must free with xr_debug_free_disasm()
XR_FUNC int xr_debug_get_disassembly(XrayIsolate *isolate, int frame_idx,
                                     XdapDisasmInstr **out_instrs, int *out_count);

// Get current instruction offset (PC) for frame
XR_FUNC int xr_debug_get_current_pc(XrayIsolate *isolate, int frame_idx);

// Free disassembly array
XR_FUNC void xr_debug_free_disasm(XdapDisasmInstr *instrs, int count);

// ============================================================================
// Async Stack Trace API
// ============================================================================

// Get async stack trace (spawn point of current coroutine)
// Returns true if async stack is available
// Caller must NOT free the returned arrays (they point to coroutine internal data)
XR_FUNC bool xr_debug_get_async_stack(XrayIsolate *isolate, int *out_depth, const char ***out_names,
                                      const char ***out_files, int **out_lines);

// ============================================================================
// Variable Reference API (for object expansion)
// ============================================================================

// Create a variable reference, returns ID for DAP client
XR_FUNC int xr_debug_create_var_ref(XrayIsolate *isolate, XdapVarRefType type, int frame_idx,
                                    XrValue value);

// Get variable reference by ID
XR_FUNC XrDebugVarRef *xr_debug_get_var_ref(XrayIsolate *isolate, int ref_id);

// Clear all variable references (call on continue/step)
XR_FUNC void xr_debug_clear_var_refs(XrayIsolate *isolate);

// Get children of a variable reference
// Returns count, caller must free each item and the array
XR_FUNC int xr_debug_get_var_children(XrayIsolate *isolate, int ref_id, XdapVarInfo **out_vars);

// Free a single XdapVarInfo
XR_FUNC void xr_debug_var_info_free(XdapVarInfo *info);

// Free an array of XdapVarInfo (frees each element's strings and the array itself)
XR_FUNC void xr_debug_var_info_array_free(XdapVarInfo *vars, int count);

// Check if a value is expandable (has children)
XR_FUNC bool xr_debug_value_is_expandable(XrayIsolate *isolate, XrValue value);

// Get variable reference type for a value
XR_FUNC XdapVarRefType xr_debug_get_ref_type(XrValue value);

// Set variable value (returns new value string, caller must free)
XR_FUNC char *xr_debug_set_variable(XrayIsolate *isolate, int var_ref, const char *name,
                                    const char *value);

// Function breakpoints
XR_FUNC int xr_debug_add_function_breakpoint(XrayIsolate *isolate, const char *func_name,
                                             const char *condition);
XR_FUNC void xr_debug_clear_function_breakpoints(XrayIsolate *isolate);
XR_FUNC bool xr_debug_check_function_breakpoint(XrayIsolate *isolate, const char *func_name);

#endif  // XDAP_DEBUG_H
