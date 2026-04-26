/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xic_method.h - Polymorphic inline cache for method dispatch
 *
 * KEY CONCEPT:
 *   Each call site caches up to XR_IC_POLY_MAX (class, method) pairs.
 *   States: empty → monomorphic → polymorphic → megamorphic.
 *   Megamorphic skips IC and goes directly to symbol-indexed lookup.
 *
 * WHY THIS DESIGN:
 *   - Polymorphic IC handles 2-4 types without full symbol lookup
 *   - Megamorphic fallback avoids linear scan overhead for 5+ types
 *   - Classes are immutable so class pointer comparison is sufficient
 */

#ifndef XIC_METHOD_H
#define XIC_METHOD_H

#include "../runtime/value/xvalue.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xmethod.h"
#include "../base/xmalloc.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#define XR_IC_POLY_MAX 4
#define XR_MEGA_CACHE_SIZE 16
#define XR_MEGA_CACHE_MASK (XR_MEGA_CACHE_SIZE - 1)

/* ========== Megamorphic Hash Cache ========== */

// Open-addressing hash table for megamorphic call sites.
// Key = XrClass pointer, Value = XrMethod pointer.
// Linear probing with max XR_MEGA_CACHE_SIZE probes.
typedef struct XrMegaCache {
    XrClass *keys[XR_MEGA_CACHE_SIZE];
    XrMethod *values[XR_MEGA_CACHE_SIZE];
} XrMegaCache;

/* ========== Polymorphic IC Entry ========== */

typedef struct XrICEntry {
    XrClass *klass;
    XrMethod *method;
    uint32_t hit_count;  // per-entry hit counter (always-on, JIT type feedback)
} XrICEntry;

// Per-call-site polymorphic inline cache
typedef struct XrICMethod {
    XrICEntry entries[XR_IC_POLY_MAX];
    uint8_t count;            // 0..XR_IC_POLY_MAX entries filled
    uint8_t is_megamorphic;   // once set, skip IC and use direct lookup
    uint32_t total_count;     // total invocations at this call site (always-on)
    XrMegaCache *mega_cache;  // lazily allocated on megamorphic transition

#ifndef NDEBUG
    int debug_instruction_offset;
#endif
#ifdef XR_DEBUG_INLINE_CACHE
    uint64_t hits;
    uint64_t misses;
#endif
} XrICMethod;

/* ========== Inline Cache Table ========== */

typedef struct XrICMethodTable {
    XrICMethod *caches;
    int count;
    int capacity;
} XrICMethodTable;

/* ========== Cache Operations ========== */

XR_FUNC XrICMethodTable *xr_ic_method_table_new(int initial_capacity);
XR_FUNC void xr_ic_method_table_free(XrICMethodTable *table);

XR_FUNC int xr_ic_method_table_alloc(XrICMethodTable *table);

static inline XrICMethod *xr_ic_method_table_get(XrICMethodTable *table, int index) {
    if (!table || index < 0 || index >= table->count) {
        return NULL;
    }
    return &table->caches[index];
}

// Polymorphic IC lookup (hot path)
// Returns cached method on hit, or does full lookup and adds entry on miss
static inline XrMethod *xr_ic_method_lookup(XrICMethod *cache, XrClass *klass, int symbol) {
    // Megamorphic: hash cache lookup
    if (cache->is_megamorphic) {
        cache->total_count++;

        XrMegaCache *mc = cache->mega_cache;
        if (mc) {
            unsigned h = ((uintptr_t) klass >> 4) & XR_MEGA_CACHE_MASK;
            for (int p = 0; p < XR_MEGA_CACHE_SIZE; p++) {
                unsigned idx = (h + p) & XR_MEGA_CACHE_MASK;
                if (mc->keys[idx] == klass) {
                    return mc->values[idx];  // mega cache hit
                }
                if (mc->keys[idx] == NULL)
                    break;  // empty slot = miss
            }
        }

        // Mega cache miss: full lookup and insert
        XrMethod *method = xr_class_lookup_method(klass, symbol);
        if (method && mc) {
            unsigned h = ((uintptr_t) klass >> 4) & XR_MEGA_CACHE_MASK;
            for (int p = 0; p < XR_MEGA_CACHE_SIZE; p++) {
                unsigned idx = (h + p) & XR_MEGA_CACHE_MASK;
                if (mc->keys[idx] == NULL || mc->keys[idx] == klass) {
                    mc->keys[idx] = klass;
                    mc->values[idx] = method;
                    break;
                }
            }
        }
        return method;
    }

    // Scan existing entries for class match
    int n = cache->count;
    for (int i = 0; i < n; i++) {
        if (cache->entries[i].klass == klass) {
            cache->entries[i].hit_count++;
            cache->total_count++;
#ifdef XR_DEBUG_INLINE_CACHE
            cache->hits++;
#endif
            return cache->entries[i].method;
        }
    }

    // Cache miss: full lookup
    XrMethod *method = xr_class_lookup_method(klass, symbol);

#ifdef XR_DEBUG_INLINE_CACHE
    cache->misses++;
#endif

    if (method == NULL)
        return NULL;

    cache->total_count++;

    // Add entry if space available, otherwise go megamorphic
    if (n < XR_IC_POLY_MAX) {
        cache->entries[n].klass = klass;
        cache->entries[n].method = method;
        cache->entries[n].hit_count = 1;
        cache->count = (uint8_t) (n + 1);
    } else {
        cache->is_megamorphic = 1;
        // Allocate mega cache and seed with existing poly entries
        cache->mega_cache = (XrMegaCache *) xr_calloc(1, sizeof(XrMegaCache));
        if (cache->mega_cache) {
            // Seed existing poly entries into hash cache
            for (int j = 0; j < XR_IC_POLY_MAX; j++) {
                XrClass *ek = cache->entries[j].klass;
                XrMethod *em = cache->entries[j].method;
                if (ek && em) {
                    unsigned h = ((uintptr_t) ek >> 4) & XR_MEGA_CACHE_MASK;
                    for (int p = 0; p < XR_MEGA_CACHE_SIZE; p++) {
                        unsigned idx = (h + p) & XR_MEGA_CACHE_MASK;
                        if (cache->mega_cache->keys[idx] == NULL) {
                            cache->mega_cache->keys[idx] = ek;
                            cache->mega_cache->values[idx] = em;
                            break;
                        }
                    }
                }
            }
            // Also insert the new entry that caused the transition
            unsigned h = ((uintptr_t) klass >> 4) & XR_MEGA_CACHE_MASK;
            for (int p = 0; p < XR_MEGA_CACHE_SIZE; p++) {
                unsigned idx = (h + p) & XR_MEGA_CACHE_MASK;
                if (cache->mega_cache->keys[idx] == NULL) {
                    cache->mega_cache->keys[idx] = klass;
                    cache->mega_cache->values[idx] = method;
                    break;
                }
            }
        }
    }

    return method;
}

/* ========== Type Feedback API (for JIT/AOT consumers) ========== */

/*
 * IC state classification for JIT decision making.
 * JIT reads IC state to decide specialization strategy:
 *   UNINIT  → no data, skip optimization
 *   MONO    → single type, best candidate for speculative devirtualization
 *   POLY    → 2-4 types, generate type-check chain
 *   MEGA    → too many types, use generic dispatch
 */
typedef enum {
    XR_IC_STATE_UNINIT = 0,
    XR_IC_STATE_MONO,
    XR_IC_STATE_POLY,
    XR_IC_STATE_MEGA
} XrICState;

// Get IC state classification
static inline XrICState xr_ic_method_state(const XrICMethod *cache) {
    if (!cache || cache->count == 0)
        return XR_IC_STATE_UNINIT;
    if (cache->is_megamorphic)
        return XR_IC_STATE_MEGA;
    if (cache->count == 1)
        return XR_IC_STATE_MONO;
    return XR_IC_STATE_POLY;
}

// Get the dominant (most frequently hit) class at this call site
// Returns NULL if no entries. Sets *out_ratio to hit_count/total (0.0-1.0).
static inline XrClass *xr_ic_method_dominant_class(const XrICMethod *cache, double *out_ratio) {
    if (!cache || cache->count == 0) {
        if (out_ratio)
            *out_ratio = 0.0;
        return NULL;
    }
    int best = 0;
    for (int i = 1; i < cache->count; i++) {
        if (cache->entries[i].hit_count > cache->entries[best].hit_count) {
            best = i;
        }
    }
    if (out_ratio) {
        *out_ratio = cache->total_count > 0
                         ? (double) cache->entries[best].hit_count / cache->total_count
                         : 0.0;
    }
    return cache->entries[best].klass;
}

/* ========== Statistics and Debug ========== */

// Dump IC type feedback for a single call site (always available)
XR_FUNC void xr_ic_method_dump_feedback(const XrICMethod *cache, int pc_offset,
                                        const char *func_name);

// Dump all IC type feedback for a function
XR_FUNC void xr_ic_method_table_dump_feedback(const XrICMethodTable *table, const char *func_name);

#ifdef XR_DEBUG_INLINE_CACHE

XR_FUNC void xr_ic_method_print_stats(const XrICMethod *cache, const char *name);
XR_FUNC void xr_ic_method_table_print_stats(const XrICMethodTable *table);
XR_FUNC double xr_ic_method_hit_rate(const XrICMethod *cache);

#endif

#endif  // XIC_METHOD_H
