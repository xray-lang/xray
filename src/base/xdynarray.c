/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdynarray.c - Dynamic array implementation
 */

#include "xdynarray.h"
#include "xmalloc.h"
#include "xchecks.h" // XR_DCHECK
#include <string.h>

void xr_dynarray_init(XrDynArray *arr, size_t elem_size) {
    XR_DCHECK(arr != NULL, "Array must not be NULL");
    XR_DCHECK(elem_size > 0, "Element size must be positive");
    
    arr->data = NULL;
    arr->count = 0;
    arr->capacity = 0;
    arr->elem_size = elem_size;
}

void xr_dynarray_free(XrDynArray *arr) {
    if (arr == NULL) {
        return;
    }
    
    if (arr->data != NULL) {
        xr_free(arr->data);
        arr->data = NULL;
    }
    
    arr->count = 0;
    arr->capacity = 0;
}

bool xr_dynarray_reserve(XrDynArray *arr, int min_capacity) {
    XR_DCHECK(arr != NULL, "Array must not be NULL");
    
    if (arr->capacity >= min_capacity) {
        return true;
    }
    
    // 2x growth
    int new_capacity = arr->capacity == 0 ? 8 : arr->capacity;
    while (new_capacity < min_capacity) {
        new_capacity *= 2;
    }
    
    void *new_data = xr_realloc(arr->data, arr->elem_size * new_capacity);
    if (new_data == NULL) {
        return false;
    }
    
    arr->data = new_data;
    arr->capacity = new_capacity;
    return true;
}

int xr_dynarray_add_raw(XrDynArray *arr, const void *elem) {
    XR_DCHECK(arr != NULL, "Array must not be NULL");
    XR_DCHECK(elem != NULL, "Element must not be NULL");
    
    if (!xr_dynarray_reserve(arr, arr->count + 1)) {
        return -1;
    }
    
    void *dest = (char*)arr->data + (arr->count * arr->elem_size);
    memcpy(dest, elem, arr->elem_size);
    
    return arr->count++;
}

void* xr_dynarray_get_raw(const XrDynArray *arr, int index) {
    XR_DCHECK(arr != NULL, "Array must not be NULL");
    XR_DCHECK_BOUNDS(index, arr->count, "Index out of bounds");
    
    return (char*)arr->data + (index * arr->elem_size);
}

void xr_dynarray_set_raw(XrDynArray *arr, int index, const void *elem) {
    XR_DCHECK(arr != NULL, "Array must not be NULL");
    XR_DCHECK(elem != NULL, "Element must not be NULL");
    XR_DCHECK_BOUNDS(index, arr->count, "Index out of bounds");
    
    void *dest = (char*)arr->data + (index * arr->elem_size);
    memcpy(dest, elem, arr->elem_size);
}

void xr_dynarray_clear(XrDynArray *arr) {
    XR_DCHECK(arr != NULL, "Array must not be NULL");
    arr->count = 0;
}

