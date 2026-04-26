/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_scope.h - Compiler scope helper structures
 *
 * KEY CONCEPT:
 *   Stores local variable info in sequential list for linear scan lookup.
 */

#ifndef XCOMPILER_SCOPE_H
#define XCOMPILER_SCOPE_H

#include "../../runtime/object/xstring.h"
#include "../../runtime/value/xtype.h"
#include "../../base/xmalloc.h"
#include <stdbool.h>
#include <string.h>

typedef enum {
    COMPTIME_NONE = 0,
    COMPTIME_INT,
    COMPTIME_FLOAT,
    COMPTIME_BOOL,
    COMPTIME_STRING,
    COMPTIME_RANGE,
} ComptimeType;

typedef struct range {
    ComptimeType type;
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
        XrString *string_val;
        struct {
            int64_t start;
            int64_t end;
        } range;
    } as;
} ComptimeValue;

typedef struct XrLocalInfo {
    XrString *name;
    int reg;
    int depth;
    bool is_captured;      // Captured by an inner closure
    bool is_cellified;     // Wrapped in heap cell (captured mutable let)
    int ctx_slot;          // Context slot index (-1 = stack only, >=0 = allocated in Context)
    XrType *compile_type;  // Unified type from analyzer
    bool is_const;
    bool is_hoisted;       // Function name pre-declared for mutual recursion
    uint8_t storage_mode;  // 0=normal, 1=shared
    ComptimeValue comptime;

    // Closure safety tracking
    bool is_closure;
    struct XrProto *closure_proto;

    // Spill management
    int spill_slot;  // -1 if not spilled

    // RELOAD cache (optimization)
    int cached_reg;  // -1 if no cache
    int cached_gen;

    // Rematerialization
    bool can_rematerialize;
    int64_t remat_value;
} XrLocalInfo;

typedef struct XrLocalList {
    XrLocalInfo **items;
    int count;
    int capacity;
} XrLocalList;

/* Set compile_type on a local variable.
 * compile_type is the single source of truth for type info. */
static inline void local_set_compile_type(XrLocalInfo *local, XrType *ct) {
    local->compile_type = ct;
}

static inline void xr_local_list_init(XrLocalList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static inline void xr_local_list_add(XrLocalList *list, XrLocalInfo *local,
                                     void *(*alloc_fn)(size_t)) {
    if (list->count >= list->capacity) {
        int new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
        XrLocalInfo **new_items = (XrLocalInfo **) alloc_fn(sizeof(XrLocalInfo *) * new_cap);
        if (list->items) {
            memcpy(new_items, list->items, sizeof(XrLocalInfo *) * list->count);
            xr_free(list->items);
        }
        list->items = new_items;
        list->capacity = new_cap;
    }
    list->items[list->count++] = local;
}

static inline XrLocalInfo *xr_local_list_get(XrLocalList *list, int index) {
    if (index < 0 || index >= list->count)
        return NULL;
    return list->items[index];
}

static inline void xr_local_list_pop_above_depth(XrLocalList *list, int depth) {
    while (list->count > 0 && list->items[list->count - 1]->depth > depth) {
        list->count--;
    }
}

#endif  // XCOMPILER_SCOPE_H
