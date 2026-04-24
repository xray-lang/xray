/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_breakpoints.c - Breakpoint management (line, function, exception, watch)
 */

#include "xdap_debug.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/xexec_state.h"
#include "../../base/xhash.h"
#include "../../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Breakpoint Hash Table (O(1) lookup by path+line)
// ============================================================================

static uint32_t bp_hash(const char *path, int line) {
    uint32_t path_hash = xr_hash_bytes(path, strlen(path));
    uint32_t line_hash = xr_hash_int((int64_t)line);
    uint32_t combined = path_hash ^ (line_hash * 0x9e3779b9);
    return combined & (XR_BP_HASH_SIZE - 1);
}

static void bp_hash_add(XrBpHashTable *table, struct XrBreakpoint *bp) {
    uint32_t idx = bp_hash(bp->path, bp->line);
    XrBpHashEntry *entry = (XrBpHashEntry *)xr_calloc(1, sizeof(XrBpHashEntry));
    if (!entry) return;
    entry->bp = bp;
    entry->next = table->buckets[idx];
    table->buckets[idx] = entry;
}

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

struct XrBreakpoint *xr_bp_hash_find(XrBpHashTable *table, const char *path, int line) {
    uint32_t idx = bp_hash(path, line);
    for (XrBpHashEntry *entry = table->buckets[idx]; entry; entry = entry->next) {
        if (entry->bp->line == line && strcmp(entry->bp->path, path) == 0) {
            return entry->bp;
        }
    }
    return NULL;
}

void xr_bp_hash_clear(XrBpHashTable *table) {
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
// Breakpoint Field Cleanup
// ============================================================================

void xr_bp_free_fields(struct XrBreakpoint *bp) {
    if (!bp) return;
    xr_free(bp->path);
    xr_free(bp->condition);
    xr_free(bp->log_message);
    xr_free(bp->hit_condition);
}

// ============================================================================
// Line Breakpoint Management
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

            xr_bp_free_fields(bp);
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

            xr_bp_free_fields(bp);
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
    struct XrBreakpoint *bp = xr_bp_hash_find(&dbg->bp_hash, path, line);
    if (!bp || !bp->enabled) return 0;

    // Increment hit count
    bp->hit_count++;

    // Check condition if present
    if (bp->condition && bp->condition[0] != '\0') {
        if (!xr_debug_eval_condition_truthy(isolate, bp->condition)) return 0;
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
// Exception Breakpoints
// ============================================================================

void xr_debug_set_exception_breakpoints(XrayIsolate *isolate, bool uncaught, bool caught) {
    XrDebugState *dbg = (XrDebugState *)xr_isolate_get_debug_state(isolate);
    if (!dbg) return;

    dbg->break_on_uncaught = uncaught;
    dbg->break_on_caught = caught;
}

// ============================================================================
// Function Breakpoints
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
            if (!xr_debug_eval_condition_truthy(isolate, bp->condition)) continue;
            return true;
        }
    }
    return false;
}
