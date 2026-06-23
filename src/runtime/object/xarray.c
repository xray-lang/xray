/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray.c - Dynamic array implementation
 *
 * KEY CONCEPT:
 *   Array objects and element storage both live on the per-coroutine heap.
 *   Element data uses XR_TBLOB (object header + raw bytes), so release paths
 *   can account for buffers. Old data buffers are reclaimed automatically.
 *   System heap arrays (shared) still use malloc for element storage.
 *   Slices share backing store with source array (zero-copy).
 */

#include "xarray.h"
#include "../core/xr_runtime_core.h"
#include "../../shared/xr_array_core.h"
#include "../../base/xchecks.h"
#include "../mem/xalloc_unified.h"
#include "../mem/xheap.h"
#include "../mem/xsystem_heap.h"
#include "../xshared.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue_hash.h"
#include <string.h>

/* ====== Creation and Destruction ====== */

static void xr_array_retain_elements(XrArray *arr) {
    if (!arr || arr->elem_type != XR_ELEM_ANY || arr->length <= 0)
        return;
    XrValue *data = (XrValue *) arr->data;
    for (int32_t i = 0; i < arr->length; i++)
        xr_rc_retain_value(data[i]);
}

static void xr_array_release_elements(XrArray *arr, XrCoroHeap *heap) {
    if (!arr)
        return;
    if (xr_array_is_slice(arr)) {
        xr_rc_release_value(heap, XR_FROM_PTR(arr->source));
        return;
    }
    if (arr->elem_type != XR_ELEM_ANY || arr->length <= 0)
        return;
    XrValue *data = (XrValue *) arr->data;
    for (int32_t i = 0; i < arr->length; i++) {
        xr_rc_release_value(heap, data[i]);
        data[i] = xr_null();
    }
}

static bool xr_array_same_gc_object(XrValue a, XrValue b) {
    return XR_IS_PTR(a) && XR_IS_PTR(b) && XR_TO_PTR(a) == XR_TO_PTR(b);
}

static void xr_array_retain_extra_fill_refs(XrValue value, int32_t changed_slots) {
    if (!XR_VALUE_NEEDS_GC(value))
        return;
    for (int32_t i = 0; i < changed_slots; i++)
        xr_rc_retain_value(value);
}

XrArray *xr_array_new(struct XrCoroutine *coro) {
    return xr_array_with_capacity(coro, 0);
}

XrArray *xr_array_with_capacity(struct XrCoroutine *coro, int capacity) {
    return xr_array_with_capacity_typed(coro, capacity, XR_ELEM_ANY);
}

XrArray *xr_array_with_capacity_typed(struct XrCoroutine *coro, int capacity,
                                      XrArrayElemType elem_type) {
    XR_DCHECK(coro != NULL, "array_with_capacity: NULL coro");
    XR_DCHECK(capacity >= 0, "array_with_capacity: negative capacity");
    XR_DCHECK(elem_type < XR_ELEM_COUNT, "array_with_capacity: invalid elem_type");
    // Allocate on coroutine heap
    XrArray *arr = (XrArray *) xr_alloc(coro, sizeof(XrArray), XR_TARRAY);

    if (!arr) {
        return NULL;
    }

    xr_obj_header_init_type(&arr->hdr, XR_TARRAY);

    uint8_t esz = (elem_type < XR_ELEM_COUNT) ? XR_ELEM_SIZES[elem_type] : 8;
    arr->data = NULL;
    arr->length = 0;
    arr->capacity = capacity;
    arr->source = NULL;
    arr->data_storage = XR_ARRAY_DATA_HEAP;
    arr->elem_type = (uint8_t) elem_type;
    arr->elem_size = esz;
    arr->elem_tid = 0;
    arr->contains_refs = 0;
    arr->data_on_region_heap = 0;
    memset(arr->_pad, 0, sizeof(arr->_pad));

    // Allocate data as GC blob on Region heap (no free needed, GC reclaims)
    if (capacity > 0) {
        size_t data_bytes = (size_t) esz * capacity;
        XrCoroHeap *heap = xr_coro_get_heap(coro);
        if (heap) {
            arr->data = xr_coro_alloc_blob(heap, data_bytes);
            if (arr->data) {
                arr->data_on_region_heap = 1;
            }
        } else {
            // Fallback: no heap available, use malloc
            arr->data = xr_malloc(data_bytes);
        }
        if (!arr->data)
            arr->capacity = 0;
    }

    return arr;
}

// Initialize array in-place (for shared arrays on system heap)
// Object header must be set by caller
void xr_array_init_inplace(XrArray *arr, int capacity, uint8_t elem_type) {
    if (!arr)
        return;

    uint8_t esz = (elem_type < XR_ELEM_COUNT) ? XR_ELEM_SIZES[elem_type] : 8;
    arr->data = NULL;
    arr->length = 0;
    arr->capacity = capacity;
    arr->source = NULL;
    arr->data_storage = XR_ARRAY_DATA_HEAP;
    arr->elem_type = elem_type;
    arr->elem_size = esz;
    arr->elem_tid = 0;
    arr->contains_refs = 0;
    arr->data_on_region_heap = 0;  // always 0 for inplace arrays
    memset(arr->_pad, 0, sizeof(arr->_pad));

    // Allocate data (no GC accounting for system heap arrays)
    if (capacity > 0) {
        arr->data = xr_malloc((size_t) esz * capacity);
        if (!arr->data)
            arr->capacity = 0;
    }
}

/* Allocate an empty ANY-typed array directly on the shared (system) heap.
 *
 * Concurrency collection points — e.g. a supervisor scope's errors[] that the
 * owner creates but many child coroutines push into across workers — must not
 * be per-coroutine arrays: growth is accounted to the pushing child's heap while
 * the array is freed by the owner's heap, underflowing the owner's byte counter
 * (and the data buffer would be reclaimed against the wrong heap). A shared
 * array carries no per-coro accounting and is reclaimed via xr_shared_destroy. */
XrArray *xr_array_new_shared_core(XrRuntimeCore *core, int capacity) {
    XrSystemHeap *heap = core ? core->sys_heap : NULL;
    if (!heap)
        return NULL;
    XrArray *arr = (XrArray *) xr_sysheap_alloc_shared(heap, sizeof(XrArray), XR_TARRAY);
    if (!arr)
        return NULL;
    xr_array_init_inplace(arr, capacity > 0 ? capacity : 4, XR_ELEM_ANY);
    XR_OBJ_SET_STORAGE(&arr->hdr, XR_OBJ_STORAGE_SHARED);
    xr_shared_set_refc(&arr->hdr, 1);
    return arr;
}

XrArray *xr_array_from_values(struct XrCoroutine *coro, XrValue *elements, int count) {
    XR_DCHECK(coro != NULL, "array_from_values: NULL coro");
    XR_DCHECK(count >= 0, "array_from_values: negative count");
    XR_DCHECK(count == 0 || elements != NULL, "array_from_values: NULL elements with count > 0");
    XrArray *arr = xr_array_with_capacity(coro, count);
    if (!arr)
        return NULL;

    // Copy elements (always XR_ELEM_ANY)
    XrValue *data = (XrValue *) arr->data;
    for (int i = 0; i < count; i++) {
        data[i] = elements[i];
        XR_ARRAY_MARK_REFS(arr, elements[i]);
    }
    arr->length = count;

    return arr;
}

/* ====== Element Access ====== */

XrValue xr_array_get(XrArray *arr, int index) {
    XR_DCHECK(arr != NULL, "array_get: NULL array");
    XR_DCHECK(XR_OBJ_GET_TYPE(&arr->hdr) == XR_TARRAY, "array_get: object is not an array");
    // Bounds check
    if (index < 0 || index >= arr->length) {
        return xr_null();
    }

    return xr_array_get_element(arr, index);
}

// Direct set without bounds check (for multi-threaded scenarios like await all)
// Thread-safe when each thread writes to different, non-overlapping indices
void xr_array_set_direct(XrArray *arr, int index, XrValue value) {
    XR_DCHECK(arr != NULL, "array_set_direct: NULL array");
    XR_DCHECK(index >= 0 && index < arr->capacity, "array_set_direct: index out of capacity");
    XrValue old = xr_null();
    bool replacing = arr->elem_type == XR_ELEM_ANY && index < arr->length;
    if (replacing)
        old = xr_array_get_element(arr, index);
    // Caller must ensure valid index and pre-allocated capacity
    xr_array_set_element(arr, index, value);
    if (replacing)
        xr_rc_release_value(xr_current_coro_heap(), old);
}

void xr_array_set(XrArray *arr, int index, XrValue value) {
    XR_DCHECK(arr != NULL, "array_set: NULL array");
    XR_DCHECK(XR_OBJ_GET_TYPE(&arr->hdr) == XR_TARRAY, "array_set: object is not an array");
    int old_length = arr->length;
    // Negative index check
    if (index < 0) {
        return;
    }

    // Extend array if index exceeds current length
    if (index >= arr->length) {
        // Slices cannot resize
        if (xr_array_is_slice(arr)) {
            return;
        }

        xr_array_ensure_capacity(arr, index + 1);

        // Fill gap with zero
        if (arr->elem_type == XR_ELEM_ANY) {
            XrValue *data = (XrValue *) arr->data;
            for (int i = arr->length; i < index; i++) {
                data[i] = xr_null();
            }
        } else {
            // Zero-fill gap for typed arrays
            memset((uint8_t *) arr->data + (size_t) arr->length * arr->elem_size, 0,
                   (size_t) (index - arr->length) * arr->elem_size);
        }

        arr->length = index + 1;
    }

    XrValue old = xr_null();
    bool replacing = arr->elem_type == XR_ELEM_ANY && index < old_length;
    if (replacing)
        old = xr_array_get_element(arr, index);
    xr_array_set_element(arr, index, value);
    if (replacing)
        xr_rc_release_value(xr_current_coro_heap(), old);
}

int xr_array_size(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_size: NULL array");
    return arr->length;
}

/* ====== Array Modification ====== */

void xr_array_push(XrArray *arr, XrValue value) {
    XR_DCHECK(arr != NULL, "array_push: NULL array");
    XR_DCHECK(XR_OBJ_GET_TYPE(&arr->hdr) == XR_TARRAY, "array_push: object is not an array");
    // Slices cannot push
    if (xr_array_is_slice(arr)) {
        return;
    }

    if (arr->length >= arr->capacity) {
        xr_array_grow(arr);
    }

    xr_array_set_element(arr, arr->length++, value);
    XR_DCHECK(arr->length <= arr->capacity, "array_push: length > capacity after push");
}

XrValue xr_array_pop(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_pop: NULL array");
    if (arr->length == 0) {
        return xr_null();
    }

    // Slices cannot pop
    if (xr_array_is_slice(arr)) {
        return xr_null();
    }

    arr->length--;
    return xr_array_get_element(arr, arr->length);
}

void xr_array_unshift(XrArray *arr, XrValue value) {
    XR_DCHECK(arr != NULL, "array_unshift: NULL array");
    // Slices cannot unshift
    if (xr_array_is_slice(arr)) {
        return;
    }

    if (arr->length >= arr->capacity) {
        xr_array_grow(arr);
    }

    (void) xr_array_core_shift_right_one(arr->data, arr->length, arr->elem_size);

    xr_array_set_element(arr, 0, value);
    arr->length++;
}

XrValue xr_array_shift(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_shift: NULL array");
    if (arr->length == 0) {
        return xr_null();
    }

    // Slices cannot shift
    if (xr_array_is_slice(arr)) {
        return xr_null();
    }

    XrValue first = xr_array_get_element(arr, 0);

    (void) xr_array_core_shift_left_one(arr->data, arr->length, arr->elem_size);

    arr->length--;
    return first;
}

void xr_array_clear(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_clear: NULL array");
    xr_array_release_elements(arr, xr_current_coro_heap());
    arr->length = 0;
}

/* ====== Query Methods ====== */

static inline XrArrayCoreNeedle xr_array_core_needle_from_value(XrValue value) {
    if (XR_IS_INT(value))
        return xr_array_core_needle_int(XR_TO_INT(value));
    if (XR_IS_FLOAT(value))
        return xr_array_core_needle_float(XR_TO_FLOAT(value));
    if (XR_IS_BOOL(value))
        return xr_array_core_needle_bool(XR_TO_BOOL(value));
    return xr_array_core_needle_other();
}

int xr_array_index_of(XrArray *arr, XrValue value) {
    XR_DCHECK(arr != NULL, "array_index_of: NULL array");
    int handled = 0;
    int64_t typed_idx = xr_array_core_typed_index_of(
        arr->data, arr->length, arr->elem_type, xr_array_core_needle_from_value(value), &handled);
    if (handled)
        return (int) typed_idx;

    for (int i = 0; i < arr->length; i++) {
        if (xr_value_eq(xr_array_get_element(arr, i), value)) {
            return i;
        }
    }
    return -1;
}

bool xr_array_has(XrArray *arr, XrValue value) {
    return xr_array_index_of(arr, value) != -1;
}

bool xr_array_is_empty(XrArray *arr) {
    XR_DCHECK(arr != NULL, "array_is_empty: NULL array");
    return arr->length == 0;
}

// Typed fill macro: write native value directly without switch dispatch
#define TYPED_FILL(type, arr, val, start, end)                                                     \
    do {                                                                                           \
        type *d = (type *) (arr)->data;                                                            \
        type v = (val);                                                                            \
        for (int64_t _i = (start); _i < (end); _i++)                                               \
            d[_i] = v;                                                                             \
    } while (0)

void xr_array_fill(XrArray *arr, XrValue value, int64_t start, int64_t end) {
    if (!arr)
        return;
    XrArrayCoreRange range = xr_array_core_fill_range(arr->length, start, end);
    if (range.count <= 0)
        return;

    if (arr->elem_type == XR_ELEM_ANY) {
        XrValue *data = (XrValue *) arr->data;
        int32_t changed_slots = 0;
        if (XR_VALUE_NEEDS_GC(value)) {
            for (int64_t i = range.start; i < range.end; i++) {
                if (!xr_array_same_gc_object(data[i], value))
                    changed_slots++;
            }
            xr_array_retain_extra_fill_refs(value, changed_slots);
        }
        XrCoroHeap *heap = xr_current_coro_heap();
        for (int64_t i = range.start; i < range.end; i++) {
            XrValue old = data[i];
            if (xr_array_same_gc_object(old, value))
                continue;
            data[i] = value;
            xr_rc_release_value(heap, old);
        }
        XR_ARRAY_MARK_REFS(arr, value);
        return;
    }

    // Typed arrays: extract native value and fill directly
    switch (arr->elem_type) {
        case XR_ELEM_I8:
            TYPED_FILL(
                int8_t, arr,
                (int8_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_U8: {
            // Special case: memset for byte arrays
            uint8_t v =
                (uint8_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value));
            memset((uint8_t *) arr->data + range.start, v, (size_t) range.count);
            break;
        }
        case XR_ELEM_I16:
            TYPED_FILL(
                int16_t, arr,
                (int16_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_U16:
            TYPED_FILL(
                uint16_t, arr,
                (uint16_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_I32:
            TYPED_FILL(
                int32_t, arr,
                (int32_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_U32:
            TYPED_FILL(
                uint32_t, arr,
                (uint32_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_I64:
            TYPED_FILL(
                int64_t, arr,
                (int64_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_U64:
            TYPED_FILL(
                uint64_t, arr,
                (uint64_t) (XR_IS_INT(value) ? XR_TO_INT(value) : (int64_t) XR_TO_FLOAT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_F32:
            TYPED_FILL(
                float, arr,
                (float) (XR_IS_FLOAT(value) ? XR_TO_FLOAT(value) : (double) XR_TO_INT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_F64:
            TYPED_FILL(
                double, arr,
                (double) (XR_IS_FLOAT(value) ? XR_TO_FLOAT(value) : (double) XR_TO_INT(value)),
                range.start, range.end);
            break;
        case XR_ELEM_BOOL: {
            uint8_t v = xr_value_is_truthy(value) ? 1 : 0;
            memset((uint8_t *) arr->data + range.start, v, (size_t) range.count);
            break;
        }
        default:
            break;
    }
}

bool xr_array_reserve(XrArray *arr, int32_t capacity) {
    if (!arr || capacity < 0 || xr_array_is_slice(arr))
        return false;
    if (arr->capacity >= capacity)
        return true;
    xr_array_ensure_capacity(arr, capacity);
    return arr->capacity >= capacity;
}

bool xr_array_resize(XrArray *arr, int32_t length, XrValue fill) {
    if (!arr || length < 0 || xr_array_is_slice(arr))
        return false;
    int32_t old_length = arr->length;
    if (length < old_length) {
        if (arr->elem_type == XR_ELEM_ANY) {
            XrValue *data = (XrValue *) arr->data;
            XrCoroHeap *heap = xr_current_coro_heap();
            for (int32_t i = length; i < old_length; i++) {
                xr_rc_release_value(heap, data[i]);
                data[i] = xr_null();
            }
        }
        arr->length = length;
        return true;
    }
    if (length == old_length)
        return true;

    xr_array_ensure_capacity(arr, length);
    if (arr->capacity < length || !arr->data)
        return false;

    int32_t added = length - old_length;
    if (arr->elem_type == XR_ELEM_ANY)
        xr_array_retain_extra_fill_refs(fill, added);
    for (int32_t i = old_length; i < length; i++)
        xr_array_set_element(arr, i, fill);
    arr->length = length;
    if (XR_ARRAY_MAY_CONTAIN_REFS(arr)) {
        XR_ARRAY_MARK_REFS(arr, fill);
    }
    return true;
}

/* ====== Utility Methods ====== */

void xr_array_reverse(XrArray *arr) {
    if (!arr || arr->length <= 1)
        return;
    (void) xr_array_core_reverse(arr->data, arr->length, arr->elem_size);
}

XrArray *xr_array_copy(struct XrCoroutine *coro, XrArray *arr) {
    if (!arr)
        return xr_array_new(coro);
    if (arr->elem_type == XR_ELEM_ANY) {
        XrArray *new_arr = xr_array_from_values(coro, (XrValue *) arr->data, arr->length);
        xr_array_retain_elements(new_arr);
        return new_arr;
    }
    // Typed array: allocate same type and memcpy
    XrArray *new_arr =
        xr_array_with_capacity_typed(coro, arr->length, (XrArrayElemType) arr->elem_type);
    if (!new_arr)
        return NULL;
    new_arr->elem_tid = arr->elem_tid;
    if (arr->length > 0) {
        memcpy(new_arr->data, arr->data, (size_t) arr->length * arr->elem_size);
    }
    new_arr->length = arr->length;
    xr_array_retain_elements(new_arr);
    return new_arr;
}

/* ====== Internal Functions ====== */

void xr_array_grow(XrArray *arr) {
    // Slices cannot grow
    if (xr_array_is_slice(arr)) {
        return;
    }

    int64_t old_capacity = arr->capacity;
    int64_t new_capacity = old_capacity == 0 ? XR_ARRAY_INIT_CAPACITY : old_capacity * 2;

    size_t old_bytes = (size_t) arr->elem_size * old_capacity;
    size_t new_bytes = (size_t) arr->elem_size * new_capacity;

    if (arr->data_on_region_heap) {
        /* Force malloc during grow to avoid Region blob overlap.
         * GC blob allocation may return memory overlapping with
         * the old data array, making memcpy undefined behavior. */
        void *new_data = xr_malloc(new_bytes);
        if (!new_data)
            return;
        if (arr->data && old_bytes > 0) {
            memcpy(new_data, arr->data, old_bytes);
        }
        // The old blob is not swept by anything: release it to the
        // owning coroutine's freelist now that the copy is done.
        xr_coro_free_blob(xr_current_coro_heap(), arr->data);
        arr->data = new_data;
        arr->data_on_region_heap = 0;
        arr->capacity = new_capacity;
        xr_coro_heap_add_external(xr_current_coro_heap(), (int64_t) new_bytes);
    } else {
        // System heap path: realloc + external memory accounting
        void *new_data = xr_realloc(arr->data, new_bytes);
        if (!new_data)
            return;
        arr->data = new_data;
        arr->capacity = new_capacity;
        /* Shared arrays carry no per-coroutine accounting: their buffer is freed
         * via xr_shared_destroy with a NULL heap, so accounting growth to the
         * pushing coroutine's heap would skew that counter (and underflow the
         * owner on a cross-coro collection point). */
        if (!XR_OBJ_IS_SHARED(&arr->hdr))
            xr_coro_heap_add_external(xr_current_coro_heap(), (int64_t) (new_bytes - old_bytes));
    }
}

void xr_array_ensure_capacity(XrArray *arr, int min_capacity) {
    XR_DCHECK(arr != NULL, "ensure_capacity: NULL array");
    XR_DCHECK(min_capacity >= 0, "ensure_capacity: negative min_capacity");
    // Slices cannot grow
    if (xr_array_is_slice(arr)) {
        return;
    }

    if (arr->capacity >= min_capacity) {
        return;
    }

    int64_t old_capacity = arr->capacity;
    int64_t new_capacity = old_capacity;
    if (new_capacity == 0) {
        new_capacity = XR_ARRAY_INIT_CAPACITY;
    }

    while (new_capacity < min_capacity) {
        new_capacity *= 2;
    }

    size_t old_bytes = (size_t) arr->elem_size * old_capacity;
    size_t new_bytes = (size_t) arr->elem_size * new_capacity;

    if (arr->data_on_region_heap) {
        // Force malloc during ensure_capacity to avoid Region blob overlap.
        void *new_data = xr_malloc(new_bytes);
        if (!new_data)
            return;
        if (arr->data && old_bytes > 0) {
            memcpy(new_data, arr->data, old_bytes);
        }
        // The old blob is not swept by anything: release it explicitly.
        xr_coro_free_blob(xr_current_coro_heap(), arr->data);
        arr->data = new_data;
        arr->data_on_region_heap = 0;
        arr->capacity = new_capacity;
        xr_coro_heap_add_external(xr_current_coro_heap(), (int64_t) new_bytes);
    } else {
        // System heap path: realloc + external memory accounting
        void *new_data = xr_realloc(arr->data, new_bytes);
        if (!new_data)
            return;
        arr->data = new_data;
        arr->capacity = new_capacity;
        /* Shared arrays carry no per-coroutine accounting: their buffer is freed
         * via xr_shared_destroy with a NULL heap, so accounting growth to the
         * pushing coroutine's heap would skew that counter (and underflow the
         * owner on a cross-coro collection point). */
        if (!XR_OBJ_IS_SHARED(&arr->hdr))
            xr_coro_heap_add_external(xr_current_coro_heap(), (int64_t) (new_bytes - old_bytes));
    }
}

/* ====== Slice Operations (zero-copy) ====== */

// Create array slice with direct data pointer offset
// data_storage == XR_ARRAY_DATA_BORROWED marks the slice as non-resizable
XrArray *xr_array_slice(struct XrCoroutine *coro, XrArray *arr, int64_t start, int64_t end) {
    if (!coro || !arr)
        return NULL;

    XrArrayCoreRange range = xr_array_core_slice_range(arr->length, start, end);

    // Allocate slice as XR_TARRAY — slices share Array layout, distinguished by
    // data_storage == XR_ARRAY_DATA_BORROWED.
    XrArray *slice = (XrArray *) xr_alloc(coro, sizeof(XrArray), XR_TARRAY);
    if (!slice)
        return NULL;

    // Direct pointer offset (zero-copy, using elem_size)
    slice->data = (uint8_t *) arr->data + (size_t) range.start * arr->elem_size;
    slice->length = range.count;
    slice->capacity = range.count;  // Pure capacity; growth is gated by data_storage
    slice->data_storage = XR_ARRAY_DATA_BORROWED;

    // Track source for GC (chase to original if source is also a slice)
    slice->source = arr->source ? arr->source : (void *) arr;
    xr_rc_retain_value(XR_FROM_PTR(slice->source));

    // Inherit elem_type, elem_tid, and contains_refs from source
    slice->elem_type = arr->elem_type;
    slice->elem_size = arr->elem_size;
    slice->elem_tid = arr->elem_tid;
    slice->contains_refs = arr->contains_refs;
    slice->data_on_region_heap = 0;  // Slice doesn't own the blob; source array marks it
    memset(slice->_pad, 0, sizeof(slice->_pad));

    return slice;
}

// Copy slice to independent array
XrArray *xr_array_slice_to_array(struct XrCoroutine *coro, XrArray *slice) {
    if (!coro || !slice)
        return xr_array_new(coro);

    // If already an independent (owning) array, return self
    if (!xr_array_is_slice(slice)) {
        return slice;
    }

    // Copy to new array (preserving elem_type and elem_tid)
    XrArray *arr =
        xr_array_with_capacity_typed(coro, slice->length, (XrArrayElemType) slice->elem_type);
    if (!arr)
        return NULL;

    arr->elem_tid = slice->elem_tid;
    if (slice->length > 0) {
        memcpy(arr->data, slice->data, (size_t) slice->length * slice->elem_size);
    }
    arr->length = slice->length;
    xr_array_retain_elements(arr);

    return arr;
}

/* ========== GC Integration ========== */

void xr_obj_destroy_array(XrObjHeader *obj, struct XrCoroHeap *owner_heap) {
    XrArray *arr = (XrArray *) obj;
    xr_array_release_elements(arr, owner_heap);
    // Only free data if this array owns its buffer (slices borrow it)
    if (arr->data && !xr_array_is_slice(arr)) {
        if (arr->data_on_region_heap) {
            // GC blob: RC owns reclamation (no sweep), release it explicitly
            xr_coro_free_blob(owner_heap, arr->data);
            arr->data = NULL;
        } else {
            // System heap: free and update external memory accounting
            size_t data_bytes = (size_t) arr->elem_size * arr->capacity;
            xr_free(arr->data);
            arr->data = NULL;
            xr_coro_heap_sub_external(owner_heap, (int64_t) data_bytes);
        }
    }
}

/* ====== Bytes Convenience Functions ====== */

uint32_t xr_array_load_u32_le(XrArray *arr, int32_t offset, bool *ok) {
    return xr_array_core_bytes_load_u32_le(arr ? arr->data : NULL, arr ? arr->length : 0,
                                           arr ? arr->elem_type : XR_ELEM_ANY, offset, ok);
}

uint64_t xr_array_load_u64_le(XrArray *arr, int32_t offset, bool *ok) {
    return xr_array_core_bytes_load_u64_le(arr ? arr->data : NULL, arr ? arr->length : 0,
                                           arr ? arr->elem_type : XR_ELEM_ANY, offset, ok);
}

bool xr_array_bytes_copy_within(XrArray *arr, int32_t dst_offset, int32_t src_offset,
                                int32_t count) {
    return xr_array_core_bytes_copy_within(arr ? arr->data : NULL, arr ? arr->length : 0,
                                           arr ? arr->elem_type : XR_ELEM_ANY, dst_offset,
                                           src_offset, count);
}

bool xr_array_bytes_copy_from(XrArray *dst, XrArray *src, int32_t src_offset, int32_t dst_offset,
                              int32_t count) {
    return xr_array_core_bytes_copy_from(dst ? dst->data : NULL, dst ? dst->length : 0,
                                         dst ? dst->elem_type : XR_ELEM_ANY, src ? src->data : NULL,
                                         src ? src->length : 0, src ? src->elem_type : XR_ELEM_ANY,
                                         src_offset, dst_offset, count, dst == src);
}

bool xr_array_bytes_repeat_from(XrArray *arr, int32_t dst_offset, int32_t distance, int32_t count) {
    return xr_array_core_bytes_repeat_from(arr ? arr->data : NULL, arr ? arr->length : 0,
                                           arr ? arr->elem_type : XR_ELEM_ANY, dst_offset, distance,
                                           count);
}

void xr_array_append_data(XrArray *arr, const uint8_t *src_data, int32_t len) {
    if (!arr || !src_data || len <= 0)
        return;
    if (arr->elem_type != XR_ELEM_U8)
        return;

    xr_array_ensure_capacity(arr, arr->length + len);
    memcpy((uint8_t *) arr->data + arr->length, src_data, len);
    arr->length += len;
}
