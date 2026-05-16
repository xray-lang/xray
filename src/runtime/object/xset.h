/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset.h - Hash-based Set with open addressing
 *
 * KEY CONCEPT:
 *   - Open addressing with linear probing
 *   - Tombstone mechanism for deletion
 *   - Small set optimization (linear scan for <=8 elements)
 *   - 75% load factor, power-of-2 capacity
 */

#ifndef XSET_H
#define XSET_H

#include "../value/xvalue.h"
#include "../gc/xgc_header.h"
#include "xarray.h"
#include <stdint.h>
#include <stdbool.h>

/* ========== Set Entry ========== */

// state encoding: 0x00=empty, 0x7F=tombstone, 0x80-0xFF=valid (bit7=1, low7=hash prefix)
typedef struct XrSetEntry {
    XrValue value;
    uint8_t state;
} XrSetEntry;

#define XR_SET_EMPTY 0x00
#define XR_SET_TOMBSTONE 0x7F
#define XR_SET_VALID 0x80

/* ========== Set Object ========== */

typedef struct XrSet {
    XrGCHeader gc;
    uint32_t capacity;
    uint32_t count;
    uint32_t tombstones;
    XrSetEntry *entries;
    uint8_t flags;
    uint8_t elem_tid;  // XrTypeId: element type for reified generics (0=any)
    uint8_t _pad[2];   // Alignment
} XrSet;

#define XR_SET_FLAG_WEAK 0x01

/* ========== Set Parameters ========== */

#define XR_SET_MIN_CAPACITY 8
#define XR_SET_LOAD_FACTOR 0.75
#define XR_SET_GROW_FACTOR 2
#define XR_SET_SMALL_SIZE 8

/* ========== Basic Operations ========== */

struct XrCoroutine;
XR_FUNC XrSet *xr_set_new(struct XrCoroutine *coro);
XR_FUNC XrSet *xr_set_new_with_capacity(struct XrCoroutine *coro, uint32_t capacity);
XR_FUNC void xr_set_init_inplace(XrSet *set);
struct XrArray;
XR_FUNC XrSet *xr_set_from_array(struct XrCoroutine *coro, struct XrArray *arr);
XR_FUNC bool xr_set_add(XrSet *set, XrValue value);
XR_FUNC bool xr_set_has(XrSet *set, XrValue value);
XR_FUNC bool xr_set_delete(XrSet *set, XrValue value);
XR_FUNC void xr_set_clear(XrSet *set);
XR_FUNC uint32_t xr_set_size(XrSet *set);
XR_FUNC bool xr_set_is_empty(XrSet *set);

/* ========== Set Operations ========== */

XR_FUNC XrSet *xr_set_union(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC XrSet *xr_set_intersection(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC XrSet *xr_set_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC XrSet *xr_set_symmetric_difference(struct XrCoroutine *coro, XrSet *set1, XrSet *set2);
XR_FUNC bool xr_set_is_subset(XrSet *set1, XrSet *set2);
XR_FUNC bool xr_set_is_superset(XrSet *set1, XrSet *set2);

/* ========== Iteration Methods ========== */

XR_FUNC XrArray *xr_set_values(struct XrCoroutine *coro, XrSet *set);

/* ========== Internal Functions ========== */

XR_FUNC XrSetEntry *xr_set_find_entry(XrSet *set, XrValue value, uint32_t *out_index);
XR_FUNC void xr_set_resize(XrSet *set, uint32_t new_capacity);
XR_FUNC XrSetEntry *xr_set_find_linear(XrSet *set, XrValue value, uint32_t *out_index);

#endif  // XSET_H
