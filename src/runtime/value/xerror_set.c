/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror_set.c - Error set type implementation
 */

#include "xerror_set.h"
#include "xtype.h"
#include "xtype_pool.h"
#include <string.h>
#include <stdio.h>

#define INITIAL_CAPACITY 4

/* ========== Internal Helpers ========== */

/* Find entry index for enum_type.  Returns -1 if not present. */
static int find_entry(const XrErrorSet *set, const XrType *enum_type) {
    if (!set || !enum_type)
        return -1;
    for (int i = 0; i < set->count; i++) {
        if (set->entries[i].enum_type == enum_type)
            return i;
    }
    return -1;
}

/* Ensure capacity for at least one more entry. */
static bool ensure_capacity(XrTypePool *pool, XrErrorSet *set) {
    if (set->count < set->capacity)
        return true;
    int new_cap = set->capacity == 0 ? INITIAL_CAPACITY : set->capacity * 2;
    XrErrorEntry *new_entries =
        (XrErrorEntry *) xr_pool_alloc(pool, (size_t) new_cap * sizeof(XrErrorEntry));
    if (!new_entries)
        return false;
    if (set->entries && set->count > 0)
        memcpy(new_entries, set->entries, (size_t) set->count * sizeof(XrErrorEntry));
    set->entries = new_entries;
    set->capacity = new_cap;
    return true;
}

/* ========== Construction ========== */

XrErrorSet *xr_error_set_new(XrTypePool *pool) {
    if (!pool)
        return NULL;
    XrErrorSet *set = (XrErrorSet *) xr_pool_alloc(pool, sizeof(XrErrorSet));
    if (!set)
        return NULL;
    set->entries = NULL;
    set->count = 0;
    set->capacity = 0;
    return set;
}

bool xr_error_set_add_case(XrTypePool *pool, XrErrorSet *set, XrType *enum_type, int case_index) {
    if (!pool || !set || !enum_type || case_index < 0 || case_index >= 64)
        return false;

    int idx = find_entry(set, enum_type);
    if (idx >= 0) {
        set->entries[idx].case_mask |= (1ULL << case_index);
        return true;
    }

    if (!ensure_capacity(pool, set))
        return false;

    XrErrorEntry *e = &set->entries[set->count++];
    e->enum_type = enum_type;
    e->case_mask = (1ULL << case_index);
    return true;
}

bool xr_error_set_add_all(XrTypePool *pool, XrErrorSet *set, XrType *enum_type) {
    if (!pool || !set || !enum_type)
        return false;

    int idx = find_entry(set, enum_type);
    if (idx >= 0) {
        set->entries[idx].case_mask = 0; /* 0 = all cases */
        return true;
    }

    if (!ensure_capacity(pool, set))
        return false;

    XrErrorEntry *e = &set->entries[set->count++];
    e->enum_type = enum_type;
    e->case_mask = 0; /* 0 = all cases */
    return true;
}

/* ========== Set Operations ========== */

void xr_error_set_union(XrTypePool *pool, XrErrorSet *dest, const XrErrorSet *src) {
    if (!pool || !dest || !src)
        return;
    for (int i = 0; i < src->count; i++) {
        XrType *et = src->entries[i].enum_type;
        uint64_t mask = src->entries[i].case_mask;

        int idx = find_entry(dest, et);
        if (idx >= 0) {
            if (mask == 0 || dest->entries[idx].case_mask == 0) {
                dest->entries[idx].case_mask = 0; /* all cases */
            } else {
                dest->entries[idx].case_mask |= mask;
            }
        } else {
            if (!ensure_capacity(pool, dest))
                return;
            XrErrorEntry *e = &dest->entries[dest->count++];
            e->enum_type = et;
            e->case_mask = mask;
        }
    }
}

void xr_error_set_subtract(XrErrorSet *dest, const XrErrorSet *src) {
    if (!dest || !src)
        return;
    for (int i = 0; i < src->count; i++) {
        int idx = find_entry(dest, src->entries[i].enum_type);
        if (idx < 0)
            continue;

        uint64_t src_mask = src->entries[i].case_mask;
        if (src_mask == 0) {
            /* Remove all cases of this enum: swap-remove */
            dest->entries[idx] = dest->entries[--dest->count];
        } else {
            dest->entries[idx].case_mask &= ~src_mask;
            if (dest->entries[idx].case_mask == 0) {
                dest->entries[idx] = dest->entries[--dest->count];
            }
        }
    }
}

bool xr_error_set_is_subset(const XrErrorSet *a, const XrErrorSet *b) {
    if (!a || a->count == 0)
        return true;
    if (!b || b->count == 0)
        return false;
    for (int i = 0; i < a->count; i++) {
        int idx = find_entry(b, a->entries[i].enum_type);
        if (idx < 0)
            return false;

        uint64_t a_mask = a->entries[i].case_mask;
        uint64_t b_mask = b->entries[idx].case_mask;

        if (b_mask == 0)
            continue; /* b has all cases */
        if (a_mask == 0)
            return false; /* a has all, b doesn't */
        if ((a_mask & b_mask) != a_mask)
            return false;
    }
    return true;
}

bool xr_error_set_equals(const XrErrorSet *a, const XrErrorSet *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return (xr_error_set_is_empty(a) && xr_error_set_is_empty(b));
    if (a->count != b->count)
        return false;
    return xr_error_set_is_subset(a, b) && xr_error_set_is_subset(b, a);
}

/* ========== Query ========== */

bool xr_error_set_contains_enum(const XrErrorSet *set, const XrType *enum_type) {
    return find_entry(set, enum_type) >= 0;
}

uint64_t xr_error_set_get_mask(const XrErrorSet *set, const XrType *enum_type) {
    int idx = find_entry(set, enum_type);
    return idx >= 0 ? set->entries[idx].case_mask : 0;
}

/* ========== Debug ========== */

const char *xr_error_set_to_string(XrTypePool *pool, const XrErrorSet *set) {
    if (!pool)
        return "<error_set:null_pool>";
    if (!set || set->count == 0)
        return "()";

    char buf[512];
    int pos = 0;

    for (int i = 0; i < set->count && pos < (int) sizeof(buf) - 16; i++) {
        if (i > 0) {
            pos += snprintf(buf + pos, sizeof(buf) - (size_t) pos, " | ");
        }
        const char *name =
            set->entries[i].enum_type ? set->entries[i].enum_type->enum_type.enum_name : "?";
        pos += snprintf(buf + pos, sizeof(buf) - (size_t) pos, "%s", name ? name : "?");
    }

    return xr_pool_strdup(pool, buf);
}
