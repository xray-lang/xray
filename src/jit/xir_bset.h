/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_bset.h - Dynamic bitset for XIR analysis passes
 *
 * KEY CONCEPT:
 *   Compact bitset over vreg indices. Used by liveness analysis,
 *   interference graphs, and any pass that tracks sets of vregs.
 *   Heap-allocated word array; all operations are O(n/64).
 *
 * WHY THIS DESIGN:
 *   - API (bsinit/bsset/bsclr/bshas/bsunion/bsinter/bsdiff)
 *   - Dynamic sizing: works for any vreg count without compile-time limit
 *   - Header-only inline implementation for zero call overhead
 */

#ifndef XIR_BSET_H
#define XIR_BSET_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "../base/xchecks.h"
#include "../base/xmalloc.h"

#define XIR_BSET_BITS 64

typedef struct {
    uint32_t  nw; // number of 64-bit words
    uint64_t *w; // heap-allocated word array
} XirBSet;

/* ========== Lifecycle ========== */

static inline void xir_bset_init(XirBSet *bs, uint32_t max_bits) {
    bs->nw = (max_bits + XIR_BSET_BITS - 1) / XIR_BSET_BITS;
    if (bs->nw == 0) bs->nw = 1;
    bs->w = (uint64_t *)xr_calloc(bs->nw, sizeof(uint64_t));
}

static inline void xir_bset_free(XirBSet *bs) {
    xr_free(bs->w);
    bs->w = NULL;
    bs->nw = 0;
}

/* ========== Basic Operations ========== */

static inline void xir_bset_zero(XirBSet *bs) {
    memset(bs->w, 0, bs->nw * sizeof(uint64_t));
}

static inline void xir_bset_fill(XirBSet *bs) {
    memset(bs->w, 0xFF, bs->nw * sizeof(uint64_t));
}

static inline bool xir_bset_has(const XirBSet *bs, uint32_t bit) {
    uint32_t wi = bit / XIR_BSET_BITS;
    XR_DCHECK(wi < bs->nw, "bset_has: bit out of range");
    return (bs->w[wi] & ((uint64_t)1 << (bit % XIR_BSET_BITS))) != 0;
}

static inline void xir_bset_set(XirBSet *bs, uint32_t bit) {
    uint32_t wi = bit / XIR_BSET_BITS;
    XR_DCHECK(wi < bs->nw, "bset_set: bit out of range");
    bs->w[wi] |= (uint64_t)1 << (bit % XIR_BSET_BITS);
}

static inline void xir_bset_clr(XirBSet *bs, uint32_t bit) {
    uint32_t wi = bit / XIR_BSET_BITS;
    XR_DCHECK(wi < bs->nw, "bset_clr: bit out of range");
    bs->w[wi] &= ~((uint64_t)1 << (bit % XIR_BSET_BITS));
}

/* ========== Set Operations ========== */

// dst = dst | src
static inline void xir_bset_union(XirBSet *dst, const XirBSet *src) {
    uint32_t n = dst->nw < src->nw ? dst->nw : src->nw;
    for (uint32_t i = 0; i < n; i++)
        dst->w[i] |= src->w[i];
}

// dst = dst & src
static inline void xir_bset_inter(XirBSet *dst, const XirBSet *src) {
    uint32_t n = dst->nw < src->nw ? dst->nw : src->nw;
    for (uint32_t i = 0; i < n; i++)
        dst->w[i] &= src->w[i];
    // clear words beyond src
    for (uint32_t i = n; i < dst->nw; i++)
        dst->w[i] = 0;
}

// dst = dst \ src (set difference)
static inline void xir_bset_diff(XirBSet *dst, const XirBSet *src) {
    uint32_t n = dst->nw < src->nw ? dst->nw : src->nw;
    for (uint32_t i = 0; i < n; i++)
        dst->w[i] &= ~src->w[i];
}

// dst = src
static inline void xir_bset_copy(XirBSet *dst, const XirBSet *src) {
    uint32_t n = dst->nw < src->nw ? dst->nw : src->nw;
    memcpy(dst->w, src->w, n * sizeof(uint64_t));
    // zero out extra words in dst
    for (uint32_t i = n; i < dst->nw; i++)
        dst->w[i] = 0;
}

/* ========== Queries ========== */

static inline bool xir_bset_equal(const XirBSet *a, const XirBSet *b) {
    uint32_t n = a->nw < b->nw ? a->nw : b->nw;
    for (uint32_t i = 0; i < n; i++)
        if (a->w[i] != b->w[i]) return false;
    // check trailing words are zero
    for (uint32_t i = n; i < a->nw; i++)
        if (a->w[i] != 0) return false;
    for (uint32_t i = n; i < b->nw; i++)
        if (b->w[i] != 0) return false;
    return true;
}

static inline bool xir_bset_empty(const XirBSet *bs) {
    for (uint32_t i = 0; i < bs->nw; i++)
        if (bs->w[i] != 0) return false;
    return true;
}

static inline uint32_t xir_bset_count(const XirBSet *bs) {
    uint32_t c = 0;
    for (uint32_t i = 0; i < bs->nw; i++)
        c += (uint32_t)__builtin_popcountll(bs->w[i]);
    return c;
}

/* ========== Iteration ========== */

/*
 * Iterate over set bits. Call with *iter = 0 initially.
 * Returns the next set bit index, or -1 when done.
 *
 * Usage:
 *   int iter = 0;
 *   int bit;
 *   while ((bit = xir_bset_iter(bs, &iter)) >= 0) {
 *       // process bit
 *   }
 */
static inline int xir_bset_iter(const XirBSet *bs, int *iter) {
    uint32_t total = bs->nw * XIR_BSET_BITS;
    for (uint32_t i = (uint32_t)*iter; i < total; i++) {
        uint32_t wi = i / XIR_BSET_BITS;
        uint32_t bi = i % XIR_BSET_BITS;
        // skip to next word if remaining bits in this word are zero
        if (bi == 0 && bs->w[wi] == 0) {
            i += XIR_BSET_BITS - 1;
            continue;
        }
        if (bs->w[wi] & ((uint64_t)1 << bi)) {
            *iter = (int)(i + 1);
            return (int)i;
        }
    }
    return -1;
}

#endif // XIR_BSET_H
