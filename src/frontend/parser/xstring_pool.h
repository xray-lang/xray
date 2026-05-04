/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_pool.h - Compile-time string deduplication pool
 *
 * KEY CONCEPT:
 *   An arena-backed open-addressing hash table that deduplicates
 *   string literals during parsing. Identical strings share the same
 *   arena pointer, reducing memory usage and enabling cheap pointer
 *   comparison in the analyzer.
 *
 *   Lifetime: created before parsing, destroyed with the arena.
 *   Not thread-safe — used only on the main parse thread.
 */

#ifndef XSTRING_POOL_H
#define XSTRING_POOL_H

#include "../../base/xdefs.h"
#include <stddef.h>

struct XrArena;

/* Opaque pool handle. Implementation in xstring_pool.c. */
typedef struct XrCompileStringPool XrCompileStringPool;

/* Create a new pool. All internal storage is arena-allocated.
 * |arena| must outlive every returned string pointer. */
XR_FUNC XrCompileStringPool *xr_string_pool_new(struct XrArena *arena);

/* Intern a NUL-terminated string. Returns the canonical pointer:
 *   - If the string is already in the pool, returns the existing pointer.
 *   - Otherwise, copies it into the arena, inserts it, and returns the copy.
 * NULL input returns NULL. */
XR_FUNC const char *xr_string_pool_intern(XrCompileStringPool *pool,
                                           const char *str);

/* Intern a length-delimited string (may contain embedded NULs, but the
 * pool stores up to |len| bytes followed by a NUL terminator). */
XR_FUNC const char *xr_string_pool_intern_len(XrCompileStringPool *pool,
                                               const char *str, size_t len);

/* Number of unique strings currently in the pool (for diagnostics). */
XR_FUNC size_t xr_string_pool_count(const XrCompileStringPool *pool);

#endif  // XSTRING_POOL_H
