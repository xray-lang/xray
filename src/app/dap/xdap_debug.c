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

// Frame context helper — eliminates repeated coro/vm_state pattern
typedef struct {
    XrBcCallFrame *frames;
    XrValue *stack;
    int frame_count;
} XrDebugFrameCtx;

static bool debug_get_frame_ctx(XrayIsolate *isolate, XrDebugFrameCtx *out) {
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

// Free a single breakpoint's owned strings
static void bp_free_fields(struct XrBreakpoint *bp) {
    xr_free(bp->path);
    xr_free(bp->condition);
    xr_free(bp->log_message);
    xr_free(bp->hit_condition);
}

// Evaluate condition string and return truthiness
// Used by both line breakpoints and function breakpoints
static bool debug_eval_condition_truthy(XrayIsolate *isolate, const char *condition) {
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
// VM Debug Hook Callbacks (registered with core)
// ============================================================================

static bool hook_check_breakpoint(XrayIsolate *isolate, const char *path, int line) {
    return xr_debug_check_breakpoint(isolate, path, line);
}

static void hook_on_breakpoint_hit(XrayIsolate *isolate, const char *path, int line,
                                    XrClosure *closure, XrBcCallFrame *frame) {
    xr_debug_on_breakpoint_hit(isolate, path, line, closure, frame);
}

static XrDebugAction hook_get_action(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    return dbg ? dbg->current_action : XR_DBG_ACTION_CONTINUE;
}

static int hook_get_step_depth(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    return dbg ? dbg->step_depth : 0;
}

static void hook_set_last_line(XrayIsolate *isolate, int line) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (dbg) dbg->last_line = line;
}

static int hook_get_last_line(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    return dbg ? dbg->last_line : -1;
}

static bool hook_is_enabled(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    return dbg && dbg->enabled;
}

// Static hooks structure (all hook functions are stateless - they access
// per-isolate debug_state via isolate parameter, safe for multi-isolate use)
static XrDebugHooks g_debug_hooks = {
    .check_breakpoint = hook_check_breakpoint,
    .on_breakpoint_hit = hook_on_breakpoint_hit,
    .get_action = hook_get_action,
    .get_step_depth = hook_get_step_depth,
    .set_last_line = hook_set_last_line,
    .get_last_line = hook_get_last_line,
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
    (void)isolate;
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
        int count = xr_json_field_count(json);
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
// Forward declarations for hash table functions
// ============================================================================

static void bp_hash_clear(XrBpHashTable *table);
static void bp_hash_add(XrBpHashTable *table, struct XrBreakpoint *bp);
static void bp_hash_remove(XrBpHashTable *table, struct XrBreakpoint *bp);
static struct XrBreakpoint *bp_hash_find(XrBpHashTable *table, const char *path, int line);

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
    bp_hash_clear(&dbg->bp_hash);
    
    // Free breakpoints
    struct XrBreakpoint *bp = dbg->breakpoints;
    while (bp) {
        struct XrBreakpoint *next = bp->next;
        bp_free_fields(bp);
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

void xr_debug_set_hook(XrayIsolate *isolate, XrDebugHookFn hook, void *userdata) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    dbg->hook = hook;
    dbg->hook_userdata = userdata;
}

void xr_debug_enable(XrayIsolate *isolate, bool enable) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    dbg->enabled = enable;
}

// Called by VM when breakpoint is hit
void xr_debug_on_breakpoint_hit(XrayIsolate *isolate, const char *path, int line,
                                 XrClosure *closure, XrBcCallFrame *frame) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    // Set paused state
    dbg->current_action = XR_DBG_ACTION_BREAK;
    xr_free(dbg->last_path);
    dbg->last_path = path ? xr_strdup(path) : NULL;
    dbg->last_line = line;
    
    // Record function context from closure
    xr_free(dbg->last_func_name);
    dbg->last_func_name = NULL;
    if (closure && closure->proto && closure->proto->name) {
        dbg->last_func_name = xr_strdup(XR_STRING_CHARS(closure->proto->name));
    }
    
    // Record frame depth for step baseline
    if (frame) {
        XrCoroutine *coro = xr_debug_get_coro(isolate);
        dbg->step_depth = coro ? coro->vm_ctx.frame_count
                               : xr_isolate_get_vm_state(isolate)->frame_count;
    }
}

// ============================================================================
// Breakpoint Hash Table (for O(1) lookup)
// ============================================================================

// Hash function using project-standard xhash.h
static uint32_t bp_hash(const char *path, int line) {
    uint32_t path_hash = xr_hash_bytes(path, strlen(path));
    uint32_t line_hash = xr_hash_int((int64_t)line);
    // Combine hashes using XOR and mix
    uint32_t combined = path_hash ^ (line_hash * 0x9e3779b9);
    return combined & (XR_BP_HASH_SIZE - 1);
}

// Add breakpoint to hash table
static void bp_hash_add(XrBpHashTable *table, struct XrBreakpoint *bp) {
    uint32_t idx = bp_hash(bp->path, bp->line);
    XrBpHashEntry *entry = (XrBpHashEntry *)xr_calloc(1, sizeof(XrBpHashEntry));
    if (!entry) return;
    entry->bp = bp;
    entry->next = table->buckets[idx];
    table->buckets[idx] = entry;
}

// Remove breakpoint from hash table
static void bp_hash_remove(XrBpHashTable *table, struct XrBreakpoint *bp) {
    uint32_t idx = bp_hash(bp->path, bp->line);
    XrBpHashEntry **pp = &table->buckets[idx];
    while (*pp) {
        if ((*pp)->bp == bp) {
            XrBpHashEntry *entry = *pp;
            *pp = entry->next;
            xr_free(entry);
            return;
        }
        pp = &(*pp)->next;
    }
}

// Find breakpoint in hash table (O(1) average case)
static struct XrBreakpoint *bp_hash_find(XrBpHashTable *table, const char *path, int line) {
    uint32_t idx = bp_hash(path, line);
    for (XrBpHashEntry *entry = table->buckets[idx]; entry; entry = entry->next) {
        if (entry->bp->line == line && strcmp(entry->bp->path, path) == 0) {
            return entry->bp;
        }
    }
    return NULL;
}

// Clear all entries from hash table
static void bp_hash_clear(XrBpHashTable *table) {
    for (int i = 0; i < XR_BP_HASH_SIZE; i++) {
        XrBpHashEntry *entry = table->buckets[i];
        while (entry) {
            XrBpHashEntry *next = entry->next;
            xr_free(entry);
            entry = next;
        }
        table->buckets[i] = NULL;
    }
}

// ============================================================================
// Breakpoint Management
// ============================================================================

int xr_debug_add_breakpoint(XrayIsolate *isolate, const char *path, int line, const char *condition) {
    return xr_debug_add_breakpoint_ex(isolate, path, line, condition, NULL, NULL);
}

int xr_debug_add_breakpoint_ex(XrayIsolate *isolate, const char *path, int line,
                                const char *condition, const char *log_message,
                                const char *hit_condition) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg || !path) return -1;
    
    struct XrBreakpoint *bp = (struct XrBreakpoint *)xr_calloc(1, sizeof(struct XrBreakpoint));
    if (!bp) return -1;
    
    bp->id = dbg->next_bp_id++;
    bp->path = xr_strdup(path);
    if (!bp->path) {
        xr_free(bp);
        return -1;
    }
    bp->line = line;
    bp->enabled = true;
    bp->hit_count = 0;
    
    // Optional fields - if strdup fails, cleanup and return error
    if (condition) {
        bp->condition = xr_strdup(condition);
        if (!bp->condition) goto cleanup;
    }
    if (log_message) {
        bp->log_message = xr_strdup(log_message);
        if (!bp->log_message) goto cleanup;
    }
    if (hit_condition) {
        bp->hit_condition = xr_strdup(hit_condition);
        if (!bp->hit_condition) goto cleanup;
    }
    
    // Add to list
    bp->next = dbg->breakpoints;
    dbg->breakpoints = bp;
    
    // Add to hash table for fast lookup
    bp_hash_add(&dbg->bp_hash, bp);
    
    return bp->id;
    
cleanup:
    xr_free(bp->path);
    xr_free(bp->condition);
    xr_free(bp->log_message);
    xr_free(bp->hit_condition);
    xr_free(bp);
    return -1;
}

bool xr_debug_remove_breakpoint(XrayIsolate *isolate, int id) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return false;
    
    struct XrBreakpoint **pp = &dbg->breakpoints;
    while (*pp) {
        if ((*pp)->id == id) {
            struct XrBreakpoint *bp = *pp;
            *pp = bp->next;
            
            // Remove from hash table
            bp_hash_remove(&dbg->bp_hash, bp);
            
            bp_free_fields(bp);
            xr_free(bp);
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

void xr_debug_clear_breakpoints(XrayIsolate *isolate, const char *path) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    struct XrBreakpoint **pp = &dbg->breakpoints;
    while (*pp) {
        if (path == NULL || strcmp((*pp)->path, path) == 0) {
            struct XrBreakpoint *bp = *pp;
            *pp = bp->next;
            
            // Remove from hash table
            bp_hash_remove(&dbg->bp_hash, bp);
            
            bp_free_fields(bp);
            xr_free(bp);
        } else {
            pp = &(*pp)->next;
        }
    }
}

// Helper: check if hit condition is satisfied
static bool check_hit_condition(const char *hit_condition, int hit_count) {
    if (!hit_condition || hit_condition[0] == '\0') return true;
    
    // Parse hit condition: ">N", ">=N", "==N", "%N" (every Nth hit)
    const char *p = hit_condition;
    while (*p == ' ') p++;
    
    char op[3] = {0};
    int i = 0;
    while (*p && !(*p >= '0' && *p <= '9') && i < 2) {
        op[i++] = *p++;
    }
    
    int n = atoi(p);
    
    if (strcmp(op, ">") == 0) return hit_count > n;
    if (strcmp(op, ">=") == 0) return hit_count >= n;
    if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0) return hit_count == n;
    if (strcmp(op, "<") == 0) return hit_count < n;
    if (strcmp(op, "<=") == 0) return hit_count <= n;
    if (strcmp(op, "%") == 0) return n > 0 && (hit_count % n) == 0;
    
    // Default: treat as ">=N"
    return hit_count >= n;
}

// Helper: expand log message with {expr} placeholders
static char *expand_log_message(XrayIsolate *isolate, const char *msg) {
    if (!msg) return NULL;
    
    // Simple expansion: replace {expr} with evaluated result
    size_t result_cap = 1024;
    char *result = (char *)xr_malloc(result_cap);
    if (!result) return NULL;
    
    size_t result_len = 0;
    
    const char *p = msg;
    while (*p) {
        if (*p == '{') {
            // Find closing brace
            const char *end = strchr(p + 1, '}');
            if (end) {
                // Extract expression
                size_t expr_len = end - p - 1;
                char *expr = (char *)xr_malloc(expr_len + 1);
                if (!expr) {
                    xr_free(result);
                    return NULL;
                }
                memcpy(expr, p + 1, expr_len);
                expr[expr_len] = '\0';
                
                // Evaluate expression
                char *value = xr_debug_evaluate(isolate, expr, 0);
                xr_free(expr);
                
                // Append value
                size_t val_len = value ? strlen(value) : 0;
                if (result_len + val_len + 1 > result_cap) {
                    result_cap = (result_len + val_len + 1) * 2;
                    char *new_result = (char *)xr_realloc(result, result_cap);
                    if (!new_result) {
                        xr_free(result);
                        xr_free(value);
                        return NULL;
                    }
                    result = new_result;
                }
                if (value) {
                    memcpy(result + result_len, value, val_len);
                    result_len += val_len;
                    xr_free(value);
                }
                
                p = end + 1;
                continue;
            }
        }
        
        // Append character
        if (result_len + 2 > result_cap) {
            result_cap *= 2;
            char *new_result = (char *)xr_realloc(result, result_cap);
            if (!new_result) {
                xr_free(result);
                return NULL;
            }
            result = new_result;
        }
        result[result_len++] = *p++;
    }
    
    result[result_len] = '\0';
    return result;
}

int xr_debug_check_breakpoint_ex(XrayIsolate *isolate, const char *path, int line,
                                  char **out_log_message) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg || !path) return 0;
    
    if (out_log_message) *out_log_message = NULL;
    
    // Use hash table for O(1) lookup
    struct XrBreakpoint *bp = bp_hash_find(&dbg->bp_hash, path, line);
    if (!bp || !bp->enabled) return 0;
    
    // Increment hit count
    bp->hit_count++;
    
    // Check condition if present
    if (bp->condition && bp->condition[0] != '\0') {
        if (!debug_eval_condition_truthy(isolate, bp->condition)) return 0;
    }
    
    // Check hit condition
    if (!check_hit_condition(bp->hit_condition, bp->hit_count)) {
        return 0;
    }
    
    // If this is a logpoint, expand and return the message
    if (bp->log_message && bp->log_message[0] != '\0') {
        if (out_log_message) {
            *out_log_message = expand_log_message(isolate, bp->log_message);
        }
        return 2;  // Logpoint
    }
    
    return 1;  // Normal breakpoint
}

bool xr_debug_check_breakpoint(XrayIsolate *isolate, const char *path, int line) {
    return xr_debug_check_breakpoint_ex(isolate, path, line, NULL) == 1;
}

// ============================================================================
// Watch Expression Management
// ============================================================================

int xr_debug_add_watch(XrayIsolate *isolate, const char *expression) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg || !expression) return -1;
    
    struct XrWatch *watch = (struct XrWatch *)xr_calloc(1, sizeof(struct XrWatch));
    if (!watch) return -1;
    watch->id = dbg->next_watch_id++;
    watch->expression = xr_strdup(expression);
    
    // Add to list
    watch->next = dbg->watches;
    dbg->watches = watch;
    
    return watch->id;
}

bool xr_debug_remove_watch(XrayIsolate *isolate, int id) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return false;
    
    struct XrWatch **pp = &dbg->watches;
    while (*pp) {
        if ((*pp)->id == id) {
            struct XrWatch *w = *pp;
            *pp = w->next;
            xr_free(w->expression);
            xr_free(w);
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

void xr_debug_clear_watches(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    struct XrWatch *w = dbg->watches;
    while (w) {
        struct XrWatch *next = w->next;
        xr_free(w->expression);
        xr_free(w);
        w = next;
    }
    dbg->watches = NULL;
}

int xr_debug_get_watch_count(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return 0;
    
    int count = 0;
    for (struct XrWatch *w = dbg->watches; w; w = w->next) {
        count++;
    }
    return count;
}

bool xr_debug_get_watch(XrayIsolate *isolate, int idx, int *out_id, const char **out_expr) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return false;
    
    int i = 0;
    for (struct XrWatch *w = dbg->watches; w; w = w->next, i++) {
        if (i == idx) {
            if (out_id) *out_id = w->id;
            if (out_expr) *out_expr = w->expression;
            return true;
        }
    }
    return false;
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
    debug_get_frame_ctx(isolate, &fctx);
    return fctx.frame_count;
}

bool xr_debug_get_frame_info(XrayIsolate *isolate, int frame_idx,
                              const char **out_func_name,
                              const char **out_source,
                              int *out_line) {
    XrDebugFrameCtx fctx;
    debug_get_frame_ctx(isolate, &fctx);
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

// ============================================================================
// Variable Inspection
// ============================================================================

int xr_debug_get_local_count(XrayIsolate *isolate, int frame_idx) {
    XrDebugFrameCtx fctx;
    debug_get_frame_ctx(isolate, &fctx);
    
    if (frame_idx < 0 || frame_idx >= fctx.frame_count) {
        return 0;
    }
    
    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];
    
    if (!frame->closure || !frame->closure->proto) {
        return 0;
    }
    
    return PROTO_LOCVAR_COUNT(frame->closure->proto);
}

bool xr_debug_get_local(XrayIsolate *isolate, int frame_idx, int local_idx,
                         const char **out_name, char **out_value, char **out_type) {
    XrDebugFrameCtx fctx;
    debug_get_frame_ctx(isolate, &fctx);
    
    if (frame_idx < 0 || frame_idx >= fctx.frame_count) {
        return false;
    }
    
    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];
    
    if (!frame->closure || !frame->closure->proto) {
        return false;
    }
    
    XrProto *proto = frame->closure->proto;
    
    int locvar_count = PROTO_LOCVAR_COUNT(proto);
    if (local_idx < 0 || local_idx >= locvar_count) {
        return false;
    }
    
    // Get local name from debug info
    if (out_name) {
        XrLocVar locvar = PROTO_LOCVAR(proto, local_idx);
        if (locvar.name) {
            *out_name = locvar.name;
        } else {
            *out_name = "<unnamed>";
        }
    }
    
    // Get value from stack
    XrValue *base = fctx.stack + frame->base_offset;
    XrValue val = base[local_idx];
    
    if (out_value) {
        *out_value = xr_value_to_debug_string(isolate, val);
    }
    
    if (out_type) {
        *out_type = xr_strdup(xr_value_type_name(val));
    }
    
    return true;
}

// ============================================================================
// VM Hook Entry Point
// ============================================================================

XrDebugAction xr_debug_on_line(XrayIsolate *isolate, const char *path, int line, 
                                XrClosure *closure, XrBcCallFrame *frame) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg || !dbg->enabled) {
        return XR_DBG_ACTION_CONTINUE;
    }
    
    bool should_break = false;
    char *log_message = NULL;
    
    // NEW: Check for pause request from DAP controller
    XdapController *ctrl = (XdapController *)xray_isolate_get_userdata(isolate);
    if (ctrl && atomic_load(&ctrl->cmd_pending) && atomic_load(&ctrl->pending_cmd) == XDAP_CMD_PAUSE) {
        atomic_store(&ctrl->cmd_pending, false);
        atomic_store(&ctrl->pending_cmd, XDAP_CMD_NONE);
        should_break = true;
        
        // Notify controller of pause
        XrCoroutine *coro = xr_isolate_get_main_coro(isolate);
        if (ctrl->stopped_coro == NULL) {
            ctrl->stopped_coro = coro;
            ctrl->stopped_coro_id = coro ? coro->id : 1;
        }
        ctrl->stop_reason = XDAP_STOP_PAUSE;
    }
    
    // Check breakpoints (including conditional and logpoints)
    if (!should_break) {
        int bp_result = xr_debug_check_breakpoint_ex(isolate, path, line, &log_message);
        if (bp_result == 1) {
            // Normal breakpoint
            should_break = true;
            if (ctrl) ctrl->stop_reason = XDAP_STOP_BREAKPOINT;
        } else if (bp_result == 2) {
            // Logpoint - send output via DAP event
            if (log_message) {
                if (ctrl) {
                    char buf[1024];
                    snprintf(buf, sizeof(buf), "%s\n", log_message);
                    xdap_send_output_event(ctrl, "console", buf);
                }
                xr_free(log_message);
            }
            // Logpoint doesn't break - continue execution
            return XR_DBG_ACTION_CONTINUE;
        }
    }
    
    // Check function breakpoints (match current closure name)
    if (!should_break && closure && closure->proto && closure->proto->name) {
        const char *func_name = XR_STRING_CHARS(closure->proto->name);
        if (func_name && dbg->func_breakpoints && 
            xr_debug_check_function_breakpoint(isolate, func_name)) {
            should_break = true;
            if (ctrl) ctrl->stop_reason = XDAP_STOP_BREAKPOINT;
        }
    }
    
    // Check stepping conditions
    bool is_step_break = false;
    if (!should_break) {
        // Get frame count from coroutine (consistent with step_depth setting)
        XrCoroutine *step_coro = xr_debug_get_coro(isolate);
        int current_depth = step_coro ? step_coro->vm_ctx.frame_count : xr_isolate_get_vm_state(isolate)->frame_count;
        
        switch (dbg->current_action) {
            case XR_DBG_ACTION_STEP_IN:
                should_break = true;
                is_step_break = true;
                if (ctrl) ctrl->stop_reason = XDAP_STOP_STEP;
                break;
                
            case XR_DBG_ACTION_STEP_OVER:
                if (current_depth <= dbg->step_depth) {
                    should_break = true;
                    is_step_break = true;
                    if (ctrl) ctrl->stop_reason = XDAP_STOP_STEP;
                }
                break;
                
            case XR_DBG_ACTION_STEP_OUT:
                if (current_depth < dbg->step_depth) {
                    should_break = true;
                    is_step_break = true;
                    if (ctrl) ctrl->stop_reason = XDAP_STOP_STEP;
                }
                break;
                
            default:
                break;
        }
    }
    
    // Skip same-line only for stepping (breakpoints and pause always hit)
    if (is_step_break && dbg->last_path && dbg->last_line == line &&
        strcmp(dbg->last_path, path) == 0) {
        should_break = false;
    }
    
    if (!should_break) {
        return XR_DBG_ACTION_CONTINUE;
    }
    
    // Update last location (strdup to avoid dangling pointer on restart/free)
    xr_free(dbg->last_path);
    dbg->last_path = path ? xr_strdup(path) : NULL;
    dbg->last_line = line;
    
    // Update controller state
    if (ctrl) {
        XrCoroutine *coro = xr_isolate_get_main_coro(isolate);
        ctrl->stopped_coro = coro;
        ctrl->stopped_coro_id = coro ? coro->id : 1;
        ctrl->stopped_path = path;
        ctrl->stopped_line = line;
        ctrl->vm_state = (int)XDAP_VM_PAUSED;
        ctrl->step_mode = (int)XDAP_CMD_NONE;
    }
    
    // Call hook if registered
    if (dbg->hook) {
        XrDebugEvent event = {
            .type = XR_DBG_HOOK_LINE,
            .source_path = path,
            .line = line,
            .column = 0,
            .closure = closure,
            .frame = frame,
            .frame_depth = xr_isolate_get_vm_state(isolate)->frame_count
        };
        
        return dbg->hook(isolate, &event, dbg->hook_userdata);
    }
    
    return XR_DBG_ACTION_BREAK;
}

// Expression evaluation is in xdap_eval.c

// ============================================================================
// Exception Breakpoints
// ============================================================================

void xr_debug_set_exception_breakpoints(XrayIsolate *isolate, bool uncaught, bool caught) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    dbg->break_on_uncaught = uncaught;
    dbg->break_on_caught = caught;
}

XrDebugAction xr_debug_on_exception(XrayIsolate *isolate, const char *message, bool is_uncaught) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg || !dbg->enabled) {
        return XR_DBG_ACTION_CONTINUE;
    }
    
    bool should_break = false;
    if (is_uncaught && dbg->break_on_uncaught) {
        should_break = true;
    } else if (!is_uncaught && dbg->break_on_caught) {
        should_break = true;
    }
    
    if (!should_break) {
        return XR_DBG_ACTION_CONTINUE;
    }
    
    // Store exception info
    xr_free(dbg->exception_message);
    dbg->exception_message = message ? xr_strdup(message) : NULL;
    dbg->exception_is_uncaught = is_uncaught;
    
    return XR_DBG_ACTION_BREAK;
}

// ============================================================================
// Coroutine Debugging
// ============================================================================

int xr_debug_get_coro_count(XrayIsolate *isolate) {
    if (!xr_isolate_get_vm_state(isolate)->scheduler) return 0;
    
    XrScheduler *sched = (XrScheduler *)xr_isolate_get_vm_state(isolate)->scheduler;
    int count = 0;
    
    // Count coroutines in ready queues (all priority levels)
    for (int p = 0; p < XR_CORO_PRIORITY_COUNT; p++) {
        XrCoroutine *coro = sched->ready_head[p];
        while (coro) {
            count++;
            coro = coro->sched_link;
        }
    }
    
    // Add current coroutine if exists
    if (sched->current) count++;
    
    return count;
}

bool xr_debug_get_coro_info(XrayIsolate *isolate, int coro_idx,
                             int *out_id, const char **out_name, const char **out_state) {
    if (!xr_isolate_get_vm_state(isolate)->scheduler) return false;
    
    XrScheduler *sched = (XrScheduler *)xr_isolate_get_vm_state(isolate)->scheduler;
    int idx = 0;
    XrCoroutine *target = NULL;
    const char *state = "unknown";
    
    // Current coroutine first
    if (sched->current) {
        if (idx == coro_idx) {
            target = sched->current;
            state = "running";
        }
        idx++;
    }
    
    // Search ready queues
    if (!target) {
        for (int p = 0; p < XR_CORO_PRIORITY_COUNT && !target; p++) {
            XrCoroutine *coro = sched->ready_head[p];
            while (coro && !target) {
                if (idx == coro_idx) {
                    target = coro;
                    state = "ready";
                }
                idx++;
                coro = coro->sched_link;
            }
        }
    }
    
    if (!target) return false;
    
    if (out_id) *out_id = target->id;
    if (out_name) *out_name = target->name ? target->name : "<unnamed>";
    if (out_state) *out_state = state;
    
    return true;
}

// ============================================================================
// Variable Reference API (for object expansion)
// ============================================================================

int xr_debug_create_var_ref(XrayIsolate *isolate, XdapVarRefType type,
                             int frame_idx, XrValue value) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return 0;
    
    // Grow array if needed
    if (dbg->var_refs_count >= dbg->var_refs_capacity) {
        int new_cap = dbg->var_refs_capacity ? dbg->var_refs_capacity * 2 : 32;
        XrDebugVarRef *new_arr = (XrDebugVarRef *)xr_realloc(
            dbg->var_refs, (size_t)new_cap * sizeof(XrDebugVarRef));
        if (!new_arr) return 0;
        dbg->var_refs = new_arr;
        dbg->var_refs_capacity = new_cap;
    }
    
    int idx = dbg->var_refs_count++;
    int id = dbg->next_var_ref_id++;
    dbg->var_refs[idx].id = id;
    dbg->var_refs[idx].type = type;
    dbg->var_refs[idx].frame_idx = frame_idx;
    dbg->var_refs[idx].value = value;
    
    return id;
}

XrDebugVarRef *xr_debug_get_var_ref(XrayIsolate *isolate, int ref_id) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return NULL;
    
    // O(1) lookup: IDs are sequential from XDAP_VAR_REF_ID_BASE
    int idx = ref_id - XDAP_VAR_REF_ID_BASE;
    if (idx < 0 || idx >= dbg->var_refs_count) return NULL;
    if (dbg->var_refs[idx].id != ref_id) return NULL;
    return &dbg->var_refs[idx];
}

void xr_debug_clear_var_refs(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    dbg->var_refs_count = 0;
    dbg->next_var_ref_id = XDAP_VAR_REF_ID_BASE;
}

void xr_debug_var_info_free(XdapVarInfo *info) {
    if (!info) return;
    xr_free(info->name);
    xr_free(info->value);
    xr_free(info->type);
}

void xr_debug_var_info_array_free(XdapVarInfo *vars, int count) {
    if (!vars) return;
    for (int i = 0; i < count; i++) {
        xr_free(vars[i].name);
        xr_free(vars[i].value);
        xr_free(vars[i].type);
    }
    xr_free(vars);
}

bool xr_debug_value_is_expandable(XrayIsolate *isolate, XrValue value) {
    (void)isolate;
    if (XR_IS_ARRAY(value)) return true;
    if (XR_IS_MAP(value)) return true;
    if (XR_IS_JSON(value)) return true;
    if (XR_IS_PTR(value)) {
        XrGCHeader *hdr = XR_TO_PTR(value);
        if (hdr->type == XR_TINSTANCE) return true;
        if (hdr->type == XR_TARRAY_SLICE) return true;
    }
    return false;
}

XdapVarRefType xr_debug_get_ref_type(XrValue value) {
    if (XR_IS_ARRAY(value)) return XDAP_REF_ARRAY;
    if (XR_IS_MAP(value)) return XDAP_REF_MAP;
    if (XR_IS_JSON(value)) return XDAP_REF_OBJECT;
    if (XR_IS_PTR(value)) {
        XrGCHeader *hdr = XR_TO_PTR(value);
        if (hdr->type == XR_TINSTANCE) return XDAP_REF_INSTANCE;
        if (hdr->type == XR_TARRAY_SLICE) return XDAP_REF_ARRAY;
    }
    return XDAP_REF_INVALID;
}

// Helper: get array children
static int get_array_children(XrayIsolate *isolate, XrArray *arr, XdapVarInfo **out_vars) {
    int count = xr_array_size(arr);
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }
    
    // Limit to first 100 elements for performance
    int display_count = count > 100 ? 100 : count;
    XdapVarInfo *vars = (XdapVarInfo *)xr_calloc(display_count, sizeof(XdapVarInfo));
    
    for (int i = 0; i < display_count; i++) {
        XrValue elem = xr_array_get(arr, i);
        
        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "[%d]", i);
        vars[i].name = xr_strdup(name_buf);
        vars[i].value = xr_value_to_debug_string(isolate, elem);
        vars[i].type = xr_strdup(xr_value_type_name(elem));
        vars[i].indexed_count = count;
        
        if (xr_debug_value_is_expandable(isolate, elem)) {
            vars[i].var_ref = xr_debug_create_var_ref(isolate, 
                xr_debug_get_ref_type(elem), -1, elem);
        }
    }
    
    *out_vars = vars;
    return display_count;
}

// Helper: get map children
static int get_map_children(XrayIsolate *isolate, XrMap *map, XdapVarInfo **out_vars) {
    uint32_t count = map->count;
    if (count == 0 || xr_map_isdummy(map)) {
        *out_vars = NULL;
        return 0;
    }
    
    // Collect non-empty entries
    XdapVarInfo *vars = (XdapVarInfo *)xr_calloc(count, sizeof(XdapVarInfo));
    int idx = 0;
    uint32_t size = xr_map_sizenode(map);
    
    for (uint32_t i = 0; i < size && idx < (int)count; i++) {
        XrMapNode *node = xr_map_node(map, i);
        if (XR_MAP_NODE_EMPTY(node)) continue;
        
        // Format key
        char *key_str = xr_value_to_debug_string(isolate, node->key);
        vars[idx].name = key_str;
        vars[idx].value = xr_value_to_debug_string(isolate, node->value);
        vars[idx].type = xr_strdup(xr_value_type_name(node->value));
        vars[idx].named_count = count;
        
        if (xr_debug_value_is_expandable(isolate, node->value)) {
            vars[idx].var_ref = xr_debug_create_var_ref(isolate,
                xr_debug_get_ref_type(node->value), -1, node->value);
        }
        idx++;
    }
    
    *out_vars = vars;
    return idx;
}

// Helper: get json object children
static int get_json_children(XrayIsolate *isolate, XrJson *json, XdapVarInfo **out_vars) {
    uint16_t count = xr_json_field_count(json);
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }
    
    XdapVarInfo *vars = (XdapVarInfo *)xr_calloc(count, sizeof(XdapVarInfo));
    
    // Iterate shape fields
    XrShape *shape = xr_json_shape(json);
    for (int i = 0; i < count; i++) {
        XrValue val = xr_json_get_field_any(json, i);
        
        // Get field name from shape
        const char *field_name = NULL;
        if (shape && shape->field_symbols && i < shape->field_count) {
            SymbolId sym = shape->field_symbols[i];
            if (xr_isolate_get_symbol_table(isolate)) {
                field_name = xr_symbol_get_name_in_table(xr_isolate_get_symbol_table(isolate), sym);
            }
        }
        
        vars[i].name = xr_strdup(field_name ? field_name : "<field>");
        vars[i].value = xr_value_to_debug_string(isolate, val);
        vars[i].type = xr_strdup(xr_value_type_name(val));
        vars[i].named_count = count;
        
        if (xr_debug_value_is_expandable(isolate, val)) {
            vars[i].var_ref = xr_debug_create_var_ref(isolate,
                xr_debug_get_ref_type(val), -1, val);
        }
    }
    
    *out_vars = vars;
    return count;
}

// Helper: get instance fields
static int get_instance_children(XrayIsolate *isolate, XrInstance *inst, XdapVarInfo **out_vars) {
    if (!inst || !inst->klass) {
        *out_vars = NULL;
        return 0;
    }
    
    int count = inst->klass->field_count;
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }
    
    XdapVarInfo *vars = (XdapVarInfo *)xr_calloc(count, sizeof(XdapVarInfo));
    
    for (int i = 0; i < count; i++) {
        XrValue val = inst->fields[i];
        
        // Get field name from class field descriptor
        const char *field_name = NULL;
        XrClass *klass = inst->klass;
        if (klass && klass->fields && i < klass->field_count) {
            field_name = klass->fields[i].name;
        }
        
        vars[i].name = xr_strdup(field_name ? field_name : "<field>");
        vars[i].value = xr_value_to_debug_string(isolate, val);
        vars[i].type = xr_strdup(xr_value_type_name(val));
        vars[i].named_count = count;
        
        if (xr_debug_value_is_expandable(isolate, val)) {
            vars[i].var_ref = xr_debug_create_var_ref(isolate,
                xr_debug_get_ref_type(val), -1, val);
        }
    }
    
    *out_vars = vars;
    return count;
}

// Helper: get scope locals
static int get_scope_locals(XrayIsolate *isolate, int frame_idx, XdapVarInfo **out_vars) {
    XrDebugFrameCtx fctx;
    debug_get_frame_ctx(isolate, &fctx);
    
    if (frame_idx < 0 || frame_idx >= fctx.frame_count) {
        *out_vars = NULL;
        return 0;
    }
    
    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];
    
    if (!frame->closure || !frame->closure->proto) {
        *out_vars = NULL;
        return 0;
    }
    
    XrProto *proto = frame->closure->proto;
    int count = PROTO_LOCVAR_COUNT(proto);
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }
    
    XdapVarInfo *vars = (XdapVarInfo *)xr_calloc(count, sizeof(XdapVarInfo));
    XrValue *base = fctx.stack + frame->base_offset;
    
    for (int i = 0; i < count; i++) {
        XrLocVar locvar = PROTO_LOCVAR(proto, i);
        XrValue val = base[i];
        
        vars[i].name = xr_strdup(locvar.name ? locvar.name : "<unnamed>");
        vars[i].value = xr_value_to_debug_string(isolate, val);
        vars[i].type = xr_strdup(xr_value_type_name(val));
        
        if (xr_debug_value_is_expandable(isolate, val)) {
            vars[i].var_ref = xr_debug_create_var_ref(isolate,
                xr_debug_get_ref_type(val), frame_idx, val);
        }
    }
    
    *out_vars = vars;
    return count;
}

int xr_debug_get_var_children(XrayIsolate *isolate, int ref_id,
                               XdapVarInfo **out_vars) {
    XrDebugVarRef *ref = xr_debug_get_var_ref(isolate, ref_id);
    if (!ref) {
        *out_vars = NULL;
        return 0;
    }
    switch (ref->type) {
        case XDAP_REF_SCOPE_LOCALS:
            return get_scope_locals(isolate, ref->frame_idx, out_vars);
            
        case XDAP_REF_ARRAY:
            if (XR_IS_ARRAY(ref->value)) {
                return get_array_children(isolate, XR_TO_ARRAY(ref->value), out_vars);
            }
            // Handle ArraySlice (same structure as Array)
            if (XR_IS_PTR(ref->value)) {
                XrGCHeader *hdr = XR_TO_PTR(ref->value);
                if (hdr->type == XR_TARRAY_SLICE) {
                    return get_array_children(isolate, (XrArray *)hdr, out_vars);
                }
            }
            break;
            
        case XDAP_REF_MAP:
            if (XR_IS_MAP(ref->value)) {
                return get_map_children(isolate, XR_TO_MAP(ref->value), out_vars);
            }
            break;
            
        case XDAP_REF_OBJECT:
            if (XR_IS_PTR(ref->value)) {
                XrGCHeader *hdr = XR_TO_PTR(ref->value);
                if (hdr->type == XR_TJSON) {
                    return get_json_children(isolate, (XrJson *)hdr, out_vars);
                }
            }
            break;
            
        case XDAP_REF_INSTANCE:
            if (XR_IS_PTR(ref->value)) {
                XrGCHeader *hdr = XR_TO_PTR(ref->value);
                if (hdr->type == XR_TINSTANCE) {
                    return get_instance_children(isolate, (XrInstance *)hdr, out_vars);
                }
            }
            break;
            
        default:
            break;
    }
    
    *out_vars = NULL;
    return 0;
}

// ============================================================================
// Hot Reload (Not Implemented)
// ============================================================================
//
// WHY NOT IMPLEMENTED:
//   1. Low ROI: xray compiles in milliseconds, restart cost is negligible
//   2. High complexity: requires handling closures, call stacks, GC roots
//   3. Edge cases: changed variable layouts, removed functions being called
//   4. DAP Restart command is sufficient for debugging workflow
//
// WHAT IT WOULD REQUIRE:
//   - Re-parse and re-compile the modified file
//   - Replace XrProto in all XrClosure instances referencing the old proto
//   - Handle changed local variable count/layout (stack frame incompatibility)
//   - Handle removed/renamed functions (dangling closures)
//   - Handle changed upvalue structure (closure capture mismatch)
//   - Coordinate with GC to avoid corrupting live objects
//
// ALTERNATIVE:
//   Use DAP "restart" command which cleanly restarts the debug session.
//   For long-running servers, consider implementing function-level reload
//   that only updates top-level function definitions (simpler, less risky).
//

bool xr_debug_hot_reload(XrayIsolate *isolate, const char *path) {
    (void)isolate;
    (void)path;
    return false;
}

// ============================================================================
// Disassembly API (Phase 4)
// ============================================================================

// Format single instruction to string using opcode names from xdebug.c
static char *format_instruction(XrProto *proto, int offset) {
    XrInstruction inst = PROTO_CODE(proto, offset);
    OpCode op = GET_OPCODE(inst);
    const char *name = xr_opcode_name(op);
    
    char buf[256];
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    uint8_t c = GETARG_C(inst);
    int sbx = GETARG_sBx(inst);
    uint16_t bx = GETARG_Bx(inst);
    (void)proto;
    
    snprintf(buf, sizeof(buf), "%-16s A=%d B=%d C=%d Bx=%d sBx=%d", 
             name, a, b, c, bx, sbx);
    
    return xr_strdup(buf);
}

// Get comment for instruction (constant values, etc.)
static char *get_instruction_comment(XrProto *proto, int offset) {
    XrInstruction inst = PROTO_CODE(proto, offset);
    OpCode op = GET_OPCODE(inst);
    uint16_t bx = GETARG_Bx(inst);
    
    // Show constant value for LOADK
    if (op == OP_LOADK && bx < (uint16_t)PROTO_CONST_COUNT(proto)) {
        XrValue val = PROTO_CONSTANT(proto, bx);
        if (XR_IS_STRING(val)) {
            XrString *str = XR_TO_STRING(val);
            char buf[64];
            snprintf(buf, sizeof(buf), "; \"%.*s\"", 
                     (int)(str->length > 40 ? 40 : str->length), str->data);
            return xr_strdup(buf);
        } else if (XR_IS_INT(val)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "; %lld", (long long)XR_TO_INT(val));
            return xr_strdup(buf);
        } else if (XR_IS_FLOAT(val)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "; %g", XR_TO_FLOAT(val));
            return xr_strdup(buf);
        }
    }
    
    // Show function name for CLOSURE
    if (op == OP_CLOSURE && bx < (uint16_t)PROTO_PROTO_COUNT(proto)) {
        XrProto *child = PROTO_PROTO(proto, bx);
        if (child && child->name) {
            char buf[64];
            snprintf(buf, sizeof(buf), "; fn %s", child->name->data);
            return xr_strdup(buf);
        }
    }
    
    return NULL;
}

int xr_debug_get_disassembly(XrayIsolate *isolate, int frame_idx,
                              XdapDisasmInstr **out_instrs, int *out_count) {
    if (!isolate || !out_instrs || !out_count) return -1;
    
    *out_instrs = NULL;
    *out_count = 0;
    
    XrDebugFrameCtx fctx;
    debug_get_frame_ctx(isolate, &fctx);
    
    if (frame_idx < 0 || frame_idx >= fctx.frame_count) return -1;
    
    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];
    
    if (!frame->closure || !frame->closure->proto) return -1;
    
    XrProto *proto = frame->closure->proto;
    int code_count = PROTO_CODE_COUNT(proto);
    
    if (code_count == 0) return 0;
    
    XdapDisasmInstr *instrs = (XdapDisasmInstr *)xr_calloc(code_count, sizeof(XdapDisasmInstr));
    
    for (int i = 0; i < code_count; i++) {
        instrs[i].offset = i;
        
        // Get line number
        int lineinfo_count = DYNARRAY_COUNT(&proto->lineinfo);
        instrs[i].line = (i < lineinfo_count) ? PROTO_LINE(proto, i) : -1;
        
        // Format instruction
        instrs[i].instruction = format_instruction(proto, i);
        instrs[i].comment = get_instruction_comment(proto, i);
    }
    
    *out_instrs = instrs;
    *out_count = code_count;
    return 0;
}

int xr_debug_get_current_pc(XrayIsolate *isolate, int frame_idx) {
    if (!isolate) return -1;
    
    XrDebugFrameCtx fctx;
    debug_get_frame_ctx(isolate, &fctx);
    
    if (frame_idx < 0 || frame_idx >= fctx.frame_count) return -1;
    
    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];
    
    if (!frame->closure || !frame->closure->proto) return -1;
    
    // PC is stored as pointer, convert to offset
    XrProto *proto = frame->closure->proto;
    XrInstruction *code_start = &PROTO_CODE(proto, 0);
    int pc = (int)(frame->pc - code_start);
    
    // PC points to next instruction, so current is pc-1
    return (pc > 0) ? pc - 1 : 0;
}

void xr_debug_free_disasm(XdapDisasmInstr *instrs, int count) {
    if (!instrs) return;
    
    for (int i = 0; i < count; i++) {
        xr_free(instrs[i].instruction);
        xr_free(instrs[i].comment);
    }
    xr_free(instrs);
}

// ============================================================================
// Async Stack Trace API (lazy capture - walk parent chain)
// ============================================================================

bool xr_debug_get_async_stack(XrayIsolate *isolate, int *out_depth,
                               const char ***out_names, const char ***out_files,
                               int **out_lines) {
    if (!isolate || !out_depth) return false;
    
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return false;
    
    *out_depth = 0;
    if (out_names) *out_names = NULL;
    if (out_files) *out_files = NULL;
    if (out_lines) *out_lines = NULL;
    
    // Get current coroutine from VM state
    XrCoroutine *coro = (XrCoroutine *)xr_isolate_get_vm_state(isolate)->current_coro;
    if (!coro || !coro->parent_coro) return false;
    
    // Walk parent chain to build async stack trace
    int depth = 0;
    XrCoroutine *c = coro;
    
    while (c && depth < 8) {
        if (c->spawn_file || c->spawn_line > 0) {
            dbg->async_names[depth] = "<go>";
            dbg->async_files[depth] = c->spawn_file;
            dbg->async_lines[depth] = c->spawn_line;
            depth++;
        }
        c = c->parent_coro;
    }
    
    if (depth == 0) return false;
    
    *out_depth = depth;
    if (out_names) *out_names = dbg->async_names;
    if (out_files) *out_files = dbg->async_files;
    if (out_lines) *out_lines = dbg->async_lines;
    
    return true;
}

// ============================================================================
// Set Variable API
// ============================================================================

char *xr_debug_set_variable(XrayIsolate *isolate, int var_ref, 
                             const char *name, const char *value) {
    if (!isolate || !name || !value) return NULL;
    
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return NULL;
    
    XrDebugFrameCtx fctx;
    debug_get_frame_ctx(isolate, &fctx);
    
    // Find the variable reference
    XrDebugVarRef *ref = xr_debug_get_var_ref(isolate, var_ref);
    int frame_idx = ref ? ref->frame_idx : 0;
    
    // For scope locals, find by name
    if (!ref || ref->type == XDAP_REF_SCOPE_LOCALS) {
        if (frame_idx < 0 || frame_idx >= fctx.frame_count) return NULL;
        
        int actual_idx = fctx.frame_count - 1 - frame_idx;
        XrBcCallFrame *frame = &fctx.frames[actual_idx];
        if (!frame->closure || !frame->closure->proto) return NULL;
        
        XrProto *proto = frame->closure->proto;
        XrValue *base = fctx.stack + frame->base_offset;
        
        // Find local by name
        int locvar_count = PROTO_LOCVAR_COUNT(proto);
        for (int i = 0; i < locvar_count; i++) {
            XrLocVar locvar = PROTO_LOCVAR(proto, i);
            if (locvar.name && strcmp(locvar.name, name) == 0) {
                // Parse the new value
                XrValue new_val;
                if (strcmp(value, "null") == 0) {
                    new_val = xr_null();
                } else if (strcmp(value, "true") == 0) {
                    new_val = xr_bool(true);
                } else if (strcmp(value, "false") == 0) {
                    new_val = xr_bool(false);
                } else if (value[0] == '"') {
                    // String literal
                    size_t len = strlen(value);
                    if (len >= 2 && value[len-1] == '"') {
                        XrString *str = xr_string_intern(isolate, value + 1, len - 2, 0);
                        new_val = xr_string_value(str);
                    } else {
                        return NULL;
                    }
                } else {
                    // Try as number
                    char *endptr = NULL;
                    long long ival = strtoll(value, &endptr, 10);
                    if (*endptr == '\0') {
                        new_val = xr_int(ival);
                    } else {
                        double fval = strtod(value, &endptr);
                        if (*endptr == '\0') {
                            new_val = xr_float(fval);
                        } else {
                            return NULL;
                        }
                    }
                }
                
                // Set the value
                base[i] = new_val;
                
                // Return the new value as string
                return xr_value_to_debug_string(isolate, new_val);
            }
        }
    }
    
    return NULL;
}

// ============================================================================
// Function Breakpoints API
// ============================================================================

int xr_debug_add_function_breakpoint(XrayIsolate *isolate, const char *func_name,
                                      const char *condition) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg || !func_name) return 0;
    
    XrFuncBreakpoint *bp = (XrFuncBreakpoint *)xr_calloc(1, sizeof(XrFuncBreakpoint));
    if (!bp) return 0;
    
    bp->id = dbg->next_func_bp_id++;
    bp->func_name = xr_strdup(func_name);
    bp->condition = condition ? xr_strdup(condition) : NULL;
    
    bp->next = dbg->func_breakpoints;
    dbg->func_breakpoints = bp;
    
    return bp->id;
}

void xr_debug_clear_function_breakpoints(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;
    
    XrFuncBreakpoint *bp = dbg->func_breakpoints;
    while (bp) {
        XrFuncBreakpoint *next = bp->next;
        xr_free(bp->func_name);
        xr_free(bp->condition);
        xr_free(bp);
        bp = next;
    }
    dbg->func_breakpoints = NULL;
}

bool xr_debug_check_function_breakpoint(XrayIsolate *isolate, const char *func_name) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg || !func_name) return false;
    
    for (XrFuncBreakpoint *bp = dbg->func_breakpoints; bp; bp = bp->next) {
        if (strcmp(bp->func_name, func_name) == 0) {
            // Check condition if present
            if (!debug_eval_condition_truthy(isolate, bp->condition)) continue;
            return true;
        }
    }
    return false;
}
