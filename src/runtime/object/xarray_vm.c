/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_vm.c - VM-facing Array adapters
 */

#include "xarray_vm.h"

#include "xstring.h"
#include "../../base/xchecks.h"
#include "../../base/xmalloc.h"
#include "../../coro/xcoroutine.h"
#include "../closure/xclosure.h"
#include "../gc/xalloc_unified.h"
#include "../value/xtype_names.h"
#include "../xisolate_api.h"
#include "../../vm/xvm_string.h"
#include "../xvm_call.h"
#include <string.h>

XrArray *xr_array_new_shared(struct XrayIsolate *X, int capacity) {
    return xr_array_new_shared_core(xr_isolate_get_runtime_core(X), capacity);
}

void xr_array_foreach(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    XR_DCHECK(arr != NULL, "xr_array_foreach: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_foreach: NULL callback");
    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        xr_vm_call_closure(iso, callback, args, 1);
    }
}

static uint8_t array_map_result_tid(struct XrClosure *callback) {
    if (!callback || !callback->proto || !callback->proto->return_type_info)
        return 0;
    return xr_type_to_tid(callback->proto->return_type_info);
}

XrArray *xr_array_map(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    XR_DCHECK(arr != NULL, "xr_array_map: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_map: NULL callback");
    uint8_t elem_tid = array_map_result_tid(callback);
    XrArrayElemType elem_type = xr_tid_to_elem_type(elem_tid);
    XrArray *result = xr_array_with_capacity_typed(xr_current_coro(iso), arr->length, elem_type);
    if (!result)
        return xr_array_new(xr_current_coro(iso));
    result->elem_tid = elem_tid;

    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        xr_array_set_element(result, i, xr_vm_call_closure(iso, callback, args, 1));
    }
    result->length = arr->length;

    return result;
}

XrArray *xr_array_filter(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    XR_DCHECK(arr != NULL, "xr_array_filter: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_filter: NULL callback");
    XrArray *result = xr_array_with_capacity_typed(xr_current_coro(iso), arr->length,
                                                   (XrArrayElemType) arr->elem_type);

    for (int i = 0; i < arr->length; i++) {
        XrValue elem = xr_array_get_element(arr, i);
        XrValue args[1];
        args[0] = elem;
        XrValue test_result = xr_vm_call_closure(iso, callback, args, 1);

        if (xr_value_is_truthy(test_result)) {
            xr_rc_retain_value(elem);
            xr_array_push(result, elem);
        }
    }

    return result;
}

XrValue xr_array_reduce(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback,
                        XrValue initial) {
    XR_DCHECK(arr != NULL, "xr_array_reduce: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_reduce: NULL callback");
    XrValue accumulator = initial;

    for (int i = 0; i < arr->length; i++) {
        XrValue args[2];
        args[0] = accumulator;
        args[1] = xr_array_get_element(arr, i);
        accumulator = xr_vm_call_closure(iso, callback, args, 2);
    }

    return accumulator;
}

XrValue xr_array_find(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    XR_DCHECK(arr != NULL, "xr_array_find: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_find: NULL callback");
    for (int i = 0; i < arr->length; i++) {
        XrValue elem = xr_array_get_element(arr, i);
        XrValue args[1];
        args[0] = elem;
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (xr_value_is_truthy(result))
            return elem;
    }
    return xr_null();
}

int xr_array_find_index(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    XR_DCHECK(arr != NULL, "xr_array_find_index: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_find_index: NULL callback");
    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (xr_value_is_truthy(result))
            return i;
    }
    return -1;
}

bool xr_array_every(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    XR_DCHECK(arr != NULL, "xr_array_every: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_every: NULL callback");
    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (!xr_value_is_truthy(result))
            return false;
    }
    return true;
}

bool xr_array_some(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *callback) {
    XR_DCHECK(arr != NULL, "xr_array_some: NULL arr");
    XR_DCHECK(callback != NULL, "xr_array_some: NULL callback");
    for (int i = 0; i < arr->length; i++) {
        XrValue args[1];
        args[0] = xr_array_get_element(arr, i);
        XrValue result = xr_vm_call_closure(iso, callback, args, 1);
        if (xr_value_is_truthy(result))
            return true;
    }
    return false;
}

static int xr_value_compare_default(XrValue a, XrValue b) {
    if (XR_IS_INT(a) && XR_IS_INT(b)) {
        int64_t ia = XR_TO_INT(a), ib = XR_TO_INT(b);
        return (ia > ib) - (ia < ib);
    }
    if (XR_IS_FLOAT(a) && XR_IS_FLOAT(b)) {
        double fa = XR_TO_FLOAT(a), fb = XR_TO_FLOAT(b);
        return (fa > fb) - (fa < fb);
    }
    if (XR_IS_INT(a) && XR_IS_FLOAT(b)) {
        double fa = (double) XR_TO_INT(a), fb = XR_TO_FLOAT(b);
        return (fa > fb) - (fa < fb);
    }
    if (XR_IS_FLOAT(a) && XR_IS_INT(b)) {
        double fa = XR_TO_FLOAT(a), fb = (double) XR_TO_INT(b);
        return (fa > fb) - (fa < fb);
    }
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) {
        const char *da = xr_value_str_data(&a);
        uint32_t la = xr_value_str_len(&a);
        const char *db = xr_value_str_data(&b);
        uint32_t lb = xr_value_str_len(&b);
        int minlen = la < lb ? la : lb;
        int cmp = memcmp(da, db, (size_t) minlen);
        if (cmp != 0)
            return cmp;
        return (la > lb) - (la < lb);
    }
    return 0;
}

typedef struct {
    struct XrayIsolate *iso;
    struct XrClosure *comparator;
} XrSortCtx;

static int xr_sort_with_comparator(const void *a, const void *b, void *ctx_ptr) {
    XrSortCtx *ctx = (XrSortCtx *) ctx_ptr;
    XrValue va = *(const XrValue *) a;
    XrValue vb = *(const XrValue *) b;
    if (ctx->comparator) {
        XrValue args[2] = {va, vb};
        XrValue result = xr_vm_call_closure(ctx->iso, ctx->comparator, args, 2);
        if (XR_IS_INT(result))
            return (int) XR_TO_INT(result);
        if (XR_IS_FLOAT(result)) {
            double d = XR_TO_FLOAT(result);
            return (d > 0) - (d < 0);
        }
        return 0;
    }
    return xr_value_compare_default(va, vb);
}

#define XR_SORT_INSERTION_THRESHOLD 32

static void xr_sort_insertion(XrValue *data, int lo, int hi, XrSortCtx *ctx) {
    for (int i = lo + 1; i <= hi; i++) {
        XrValue key = data[i];
        int j = i - 1;
        while (j >= lo && xr_sort_with_comparator(&data[j], &key, ctx) > 0) {
            data[j + 1] = data[j];
            j--;
        }
        data[j + 1] = key;
    }
}

static void xr_sort_merge(XrValue *data, XrValue *tmp, int lo, int mid, int hi, XrSortCtx *ctx) {
    memcpy(tmp + lo, data + lo, (size_t) (hi - lo + 1) * sizeof(XrValue));
    int i = lo, j = mid + 1, k = lo;
    while (i <= mid && j <= hi) {
        if (xr_sort_with_comparator(&tmp[i], &tmp[j], ctx) <= 0)
            data[k++] = tmp[i++];
        else
            data[k++] = tmp[j++];
    }
    while (i <= mid)
        data[k++] = tmp[i++];
    while (j <= hi)
        data[k++] = tmp[j++];
}

static void xr_array_hybrid_sort(XrValue *data, int n, XrSortCtx *ctx) {
    if (n <= XR_SORT_INSERTION_THRESHOLD) {
        xr_sort_insertion(data, 0, n - 1, ctx);
        return;
    }

    XrValue *tmp = (XrValue *) xr_malloc((size_t) n * sizeof(XrValue));
    if (!tmp) {
        xr_sort_insertion(data, 0, n - 1, ctx);
        return;
    }

    for (int i = 0; i < n; i += XR_SORT_INSERTION_THRESHOLD) {
        int hi = i + XR_SORT_INSERTION_THRESHOLD - 1;
        if (hi >= n)
            hi = n - 1;
        xr_sort_insertion(data, i, hi, ctx);
    }

    for (int width = XR_SORT_INSERTION_THRESHOLD; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += width * 2) {
            int mid = lo + width - 1;
            int hi = lo + width * 2 - 1;
            if (mid >= n)
                break;
            if (hi >= n)
                hi = n - 1;
            xr_sort_merge(data, tmp, lo, mid, hi, ctx);
        }
    }

    xr_free(tmp);
}

#define TYPED_INSERTION(type, d, lo, hi)                                                           \
    do {                                                                                           \
        for (int _i = (lo) + 1; _i <= (hi); _i++) {                                                \
            type _key = (d)[_i];                                                                   \
            int _j = _i - 1;                                                                       \
            while (_j >= (lo) && (d)[_j] > _key) {                                                 \
                (d)[_j + 1] = (d)[_j];                                                             \
                _j--;                                                                              \
            }                                                                                      \
            (d)[_j + 1] = _key;                                                                    \
        }                                                                                          \
    } while (0)

#define TYPED_MERGE(type, d, tmp, lo, mid, hi)                                                     \
    do {                                                                                           \
        memcpy((tmp) + (lo), (d) + (lo), (size_t) ((hi) - (lo) + 1) * sizeof(type));               \
        int _i = (lo), _j = (mid) + 1, _k = (lo);                                                  \
        while (_i <= (mid) && _j <= (hi)) {                                                        \
            if ((tmp)[_i] <= (tmp)[_j])                                                            \
                (d)[_k++] = (tmp)[_i++];                                                           \
            else                                                                                   \
                (d)[_k++] = (tmp)[_j++];                                                           \
        }                                                                                          \
        while (_i <= (mid))                                                                        \
            (d)[_k++] = (tmp)[_i++];                                                               \
        while (_j <= (hi))                                                                         \
            (d)[_k++] = (tmp)[_j++];                                                               \
    } while (0)

#define TYPED_SORT(type, arr, n)                                                                   \
    do {                                                                                           \
        type *_d = (type *) (arr)->data;                                                           \
        int _n = (n);                                                                              \
        if (_n <= XR_SORT_INSERTION_THRESHOLD) {                                                   \
            TYPED_INSERTION(type, _d, 0, _n - 1);                                                  \
        } else {                                                                                   \
            type *_tmp = (type *) xr_malloc((size_t) _n * sizeof(type));                           \
            if (!_tmp) {                                                                           \
                TYPED_INSERTION(type, _d, 0, _n - 1);                                              \
                break;                                                                             \
            }                                                                                      \
            for (int _r = 0; _r < _n; _r += XR_SORT_INSERTION_THRESHOLD) {                         \
                int _hi = _r + XR_SORT_INSERTION_THRESHOLD - 1;                                    \
                if (_hi >= _n)                                                                     \
                    _hi = _n - 1;                                                                  \
                TYPED_INSERTION(type, _d, _r, _hi);                                                \
            }                                                                                      \
            for (int _w = XR_SORT_INSERTION_THRESHOLD; _w < _n; _w *= 2) {                         \
                for (int _lo = 0; _lo < _n; _lo += _w * 2) {                                       \
                    int _mid = _lo + _w - 1;                                                       \
                    int _hi2 = _lo + _w * 2 - 1;                                                   \
                    if (_mid >= _n)                                                                \
                        break;                                                                     \
                    if (_hi2 >= _n)                                                                \
                        _hi2 = _n - 1;                                                             \
                    TYPED_MERGE(type, _d, _tmp, _lo, _mid, _hi2);                                  \
                }                                                                                  \
            }                                                                                      \
            xr_free(_tmp);                                                                         \
        }                                                                                          \
    } while (0)

static void xr_array_typed_sort_with_comparator(struct XrayIsolate *iso, XrArray *arr,
                                                struct XrClosure *cmp) {
    int n = arr->length;
    XrValue *boxed = (XrValue *) xr_malloc((size_t) n * sizeof(XrValue));
    if (!boxed)
        return;
    for (int i = 0; i < n; i++)
        boxed[i] = xr_array_get_element(arr, i);
    XrSortCtx ctx = {iso, cmp};
    xr_array_hybrid_sort(boxed, n, &ctx);
    for (int i = 0; i < n; i++)
        xr_array_set_element(arr, i, boxed[i]);
    xr_free(boxed);
}

void xr_array_sort(struct XrayIsolate *iso, XrArray *arr, struct XrClosure *comparator) {
    if (!arr || arr->length <= 1)
        return;

    if (arr->elem_type == XR_ELEM_ANY) {
        XrSortCtx ctx = {iso, comparator};
        xr_array_hybrid_sort((XrValue *) arr->data, (int) arr->length, &ctx);
        return;
    }

    if (comparator) {
        xr_array_typed_sort_with_comparator(iso, arr, comparator);
        return;
    }

    int n = arr->length;
    switch (arr->elem_type) {
        case XR_ELEM_I8:
            TYPED_SORT(int8_t, arr, n);
            break;
        case XR_ELEM_U8:
            TYPED_SORT(uint8_t, arr, n);
            break;
        case XR_ELEM_I16:
            TYPED_SORT(int16_t, arr, n);
            break;
        case XR_ELEM_U16:
            TYPED_SORT(uint16_t, arr, n);
            break;
        case XR_ELEM_I32:
            TYPED_SORT(int32_t, arr, n);
            break;
        case XR_ELEM_U32:
            TYPED_SORT(uint32_t, arr, n);
            break;
        case XR_ELEM_I64:
            TYPED_SORT(int64_t, arr, n);
            break;
        case XR_ELEM_U64:
            TYPED_SORT(uint64_t, arr, n);
            break;
        case XR_ELEM_F32:
            TYPED_SORT(float, arr, n);
            break;
        case XR_ELEM_F64:
            TYPED_SORT(double, arr, n);
            break;
        default:
            break;
    }
}

struct XrString *xr_array_join(struct XrayIsolate *iso, XrArray *arr, struct XrString *delimiter) {
    if (arr == NULL || arr->length == 0)
        return xr_string_intern(iso, "", 0, 0);

    XrStrBuf *sb = xr_strbuf_tmp(iso);

    for (int i = 0; i < arr->length; i++) {
        if (i > 0 && delimiter != NULL)
            xr_strbuf_append_str(sb, delimiter);

        XrValue val = xr_array_get_element(arr, i);

        if (XR_IS_STRING(val)) {
            xr_strbuf_append_cstr(sb, xr_value_str_data(&val), xr_value_str_len(&val));
        } else if (XR_IS_INT(val)) {
            xr_strbuf_append_int(sb, XR_TO_INT(val));
        } else if (XR_IS_FLOAT(val)) {
            xr_strbuf_append_float(sb, XR_TO_FLOAT(val));
        } else if (XR_IS_BOOL(val)) {
            const char *s = XR_TO_BOOL(val) ? "true" : "false";
            xr_strbuf_append_cstr(sb, s, strlen(s));
        } else if (XR_IS_NULL(val)) {
            xr_strbuf_append_cstr(sb, "null", 4);
        } else {
            xr_strbuf_append_cstr(sb, "[object]", 8);
        }
    }

    return xr_strbuf_to_string(sb);
}

struct XrString *xr_array_to_string(struct XrayIsolate *iso, XrArray *arr) {
    if (!arr || arr->length == 0)
        return xr_string_intern(iso, "", 0, 0);
    return xr_string_intern(iso, (const char *) arr->data, arr->length, 0);
}
