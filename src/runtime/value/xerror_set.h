/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror_set.h - Error set type for the value-return error system
 *
 * KEY CONCEPT:
 *   An XrErrorSet is an ordered collection of (enum_type, case_indices)
 *   pairs representing which error cases a function may produce.
 *   NULL error_set on a function type means infallible.
 *
 * WHY THIS DESIGN:
 *   - Compact representation: small struct, arena-allocated
 *   - Supports set operations: union, difference, subset check
 *   - Decoupled from runtime: purely a compile-time construct
 *   - Enables error set inference and exhaustiveness checking
 */

#ifndef XERROR_SET_H
#define XERROR_SET_H

#include <stdbool.h>
#include <stdint.h>
#include "../../base/xdefs.h"

typedef struct XrType XrType;
typedef struct XrTypePool XrTypePool;
typedef struct XrVMRuntime XrVMRuntime;

/*
 * A single entry: one enum type and which of its cases appear.
 * `case_mask` is a bitset (up to 64 cases per enum).
 * `case_mask == 0` with a non-NULL enum_type means "all cases".
 */
typedef struct XrErrorEntry {
    XrType *enum_type;  /* XR_KIND_ENUM type */
    uint64_t case_mask; /* Bitset of case indices (0 = all) */
} XrErrorEntry;

/*
 * An error set: zero or more XrErrorEntry, arena-allocated.
 * Invariant: entries are sorted by enum_type pointer for fast merge.
 * `count == 0` means "empty error set" (function proven infallible
 * despite being syntactically marked).  NULL XrErrorSet* on a function
 * type means "infallible" (never checked).
 */
typedef struct XrErrorSet {
    XrErrorEntry *entries; /* Array of entries (pool-allocated) */
    int count;             /* Number of distinct enum types */
    int capacity;          /* Allocated capacity */
} XrErrorSet;

/* ========== Construction ========== */

/* Create an empty error set (pool-allocated). */
XR_FUNC XrErrorSet *xr_error_set_new(XrTypePool *pool);

/* Add a single enum case to the set.  case_index is the 0-based
 * index of the enum variant.  Returns false on allocation failure. */
XR_FUNC bool xr_error_set_add_case(XrTypePool *pool, XrErrorSet *set, XrType *enum_type,
                                   int case_index);

/* Add all cases of an enum type to the set. */
XR_FUNC bool xr_error_set_add_all(XrTypePool *pool, XrErrorSet *set, XrType *enum_type);

/* ========== Set Operations ========== */

/* Union: dest |= src.  All entries in src are merged into dest. */
XR_FUNC void xr_error_set_union(XrTypePool *pool, XrErrorSet *dest, const XrErrorSet *src);

/* Difference: dest -= src.  Entries in src are removed from dest.
 * Entries with an empty case_mask after removal are pruned. */
XR_FUNC void xr_error_set_subtract(XrErrorSet *dest, const XrErrorSet *src);

/* Subset check: a ⊆ b. */
XR_FUNC bool xr_error_set_is_subset(const XrErrorSet *a, const XrErrorSet *b);

/* Equality: a == b (same enum types, same case masks). */
XR_FUNC bool xr_error_set_equals(const XrErrorSet *a, const XrErrorSet *b);

/* Is the error set empty? */
static inline bool xr_error_set_is_empty(const XrErrorSet *set) {
    return !set || set->count == 0;
}

/* ========== Query ========== */

/* Check if a specific enum type appears in the set. */
XR_FUNC bool xr_error_set_contains_enum(const XrErrorSet *set, const XrType *enum_type);

/* Get the case mask for a specific enum type (0 if not present). */
XR_FUNC uint64_t xr_error_set_get_mask(const XrErrorSet *set, const XrType *enum_type);

/* ========== Debug ========== */

/* Return a human-readable string (pool-allocated) like "ParseErr | IoErr". */
XR_FUNC const char *xr_error_set_to_string(XrTypePool *pool, const XrErrorSet *set);

#endif /* XERROR_SET_H */
