/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarena_vec.h - Arena-backed dynamic array (type-safe macros)
 *
 * KEY CONCEPT:
 *   Replace fixed-size arrays (e.g. int buf[256]) with growable arrays
 *   allocated from an arena.  No individual frees needed; the arena
 *   reclaims all memory in bulk.
 *
 * Usage:
 *   XrArenaVec(int) jumps;               // declare in struct
 *   XR_AVEC_INIT(jumps);                 // zero-init
 *   XR_AVEC_PUSH(arena, jumps, 42);      // append element
 *   jumps.data[0]                         // indexed access
 *   jumps.count                           // current length
 */

#ifndef XARENA_VEC_H
#define XARENA_VEC_H

#include "xarena.h"
#include "xchecks.h"
#include <string.h>

/* Declare an arena-vec field with element type T. */
#define XrArenaVec(T) struct { T *data; int count; int cap; }

/* Zero-initialize an arena-vec. */
#define XR_AVEC_INIT(v) do { (v).data = NULL; (v).count = 0; (v).cap = 0; } while (0)

/* Push one element, growing via the arena when needed.
 * Old backing memory is not freed (arena does bulk release). */
#define XR_AVEC_PUSH(arena, v, item) do { \
    if ((v).count >= (v).cap) { \
        int _nc = (v).cap == 0 ? 8 : (v).cap * 2; \
        void *_nb = xr_arena_alloc((arena), (size_t)_nc * sizeof(*(v).data)); \
        XR_CHECK(_nb != NULL, "XR_AVEC_PUSH: arena alloc failed"); \
        if ((v).data != NULL && (v).count > 0) { \
            memcpy(_nb, (v).data, (size_t)(v).count * sizeof(*(v).data)); \
        } \
        (v).data = _nb; \
        (v).cap = _nc; \
    } \
    (v).data[(v).count++] = (item); \
} while (0)

/* Pre-allocate capacity to avoid repeated grow+copy.
 * Use when final size is known or can be estimated. */
#define XR_AVEC_RESERVE(arena, v, min_cap) do { \
    if ((v).cap < (min_cap)) { \
        int _nc = (min_cap); \
        void *_nb = xr_arena_alloc((arena), (size_t)_nc * sizeof(*(v).data)); \
        XR_CHECK(_nb != NULL, "XR_AVEC_RESERVE: arena alloc failed"); \
        if ((v).data != NULL && (v).count > 0) { \
            memcpy(_nb, (v).data, (size_t)(v).count * sizeof(*(v).data)); \
        } \
        (v).data = _nb; \
        (v).cap = _nc; \
    } \
} while (0)

#endif // XARENA_VEC_H
