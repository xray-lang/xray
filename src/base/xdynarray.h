/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdynarray.h - Generic dynamic array with type-safe macros
 *
 * KEY CONCEPT:
 *   Type-erased dynamic array. Single implementation, type-safe via macros.
 *   Growth strategy: 2x when full (amortized O(1) append).
 *
 * USAGE EXAMPLE:
 *   XrDynArray arr;
 *   DYNARRAY_INIT(&arr, int);
 *   DYNARRAY_ADD(&arr, 42, int);
 *   int val = DYNARRAY_GET(&arr, 0, int);
 *   DYNARRAY_FREE(&arr);
 *
 * USE CASES:
 *   - XrProto.code: Bytecode instruction array
 *   - XrProto.constants: Constant pool
 *   - Compiler local variable lists
 *
 * NOTE: For GC-managed arrays (xray Array type), see xarray.h
 */

#ifndef XDYNARRAY_H
#define XDYNARRAY_H

#include <stddef.h>
#include <stdbool.h>
#include "xdefs.h"

typedef struct XrDynArray {
    void *data;
    int count;
    int capacity;
    size_t elem_size;
} XrDynArray;

XR_FUNC void xr_dynarray_init(XrDynArray *arr, size_t elem_size);
XR_FUNC void xr_dynarray_free(XrDynArray *arr);
XR_FUNC bool xr_dynarray_reserve(XrDynArray *arr, int min_capacity);
XR_FUNC int xr_dynarray_add_raw(XrDynArray *arr, const void *elem);
XR_FUNC void *xr_dynarray_get_raw(const XrDynArray *arr, int index);
XR_FUNC void xr_dynarray_set_raw(XrDynArray *arr, int index, const void *elem);
XR_FUNC void xr_dynarray_clear(XrDynArray *arr);

// Type-safe generic macros
#define DYNARRAY_TYPE(T) XrDynArray
#define DYNARRAY_INIT(arr, T) xr_dynarray_init(arr, sizeof(T))

#define DYNARRAY_ADD(arr, elem, T)                                                                 \
    ({                                                                                             \
        T _tmp = (elem);                                                                           \
        xr_dynarray_add_raw(arr, &_tmp);                                                           \
    })

#define DYNARRAY_GET(arr, index, T) (*(T *) xr_dynarray_get_raw(arr, index))

#define DYNARRAY_GET_PTR(arr, index, T) ((T *) xr_dynarray_get_raw(arr, index))

#define DYNARRAY_SET(arr, index, elem, T)                                                          \
    do {                                                                                           \
        T _tmp = (elem);                                                                           \
        xr_dynarray_set_raw(arr, index, &_tmp);                                                    \
    } while (0)

#define DYNARRAY_COUNT(arr) ((arr)->count)
#define DYNARRAY_CAPACITY(arr) ((arr)->capacity)
#define DYNARRAY_FREE(arr) xr_dynarray_free(arr)
#define DYNARRAY_CLEAR(arr) xr_dynarray_clear(arr)

#define DYNARRAY_FOREACH(arr, i) for (int i = 0; i < (arr)->count; i++)

#endif  // XDYNARRAY_H
