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
#include "../mem/xalloc_unified.h"
#include "../value/xtype_names.h"
#include "../xisolate_api.h"
#include "../../shared/xr_array_core.h"
#include "../../shared/xr_float_fmt.h"
#include "../../shared/xr_sort_core.h"
#include "../xvm_call.h"
#include <stdio.h>
#include <string.h>

XrArray *xr_array_new_shared(struct XrVMRuntime *X, int capacity) {
    return xr_array_new_shared_core(xr_isolate_get_runtime_core(X), capacity);
}

void xr_array_foreach(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback) {
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

XrArray *xr_array_map(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback) {
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

XrArray *xr_array_filter(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback) {
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

XrValue xr_array_reduce(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback,
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

XrValue xr_array_find(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback) {
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

int xr_array_find_index(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback) {
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

bool xr_array_every(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback) {
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

bool xr_array_some(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *callback) {
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

typedef struct {
    struct XrVMRuntime *iso;
    struct XrClosure *comparator;
} XrSortCtx;

static int xr_array_sort_default_cmp(XrValue a, XrValue b) {
    if (XR_IS_STRING(a) && XR_IS_STRING(b)) {
        const char *a_data = xr_value_str_data(&a);
        int64_t a_len = (int64_t) xr_value_str_len(&a);
        const char *b_data = xr_value_str_data(&b);
        int64_t b_len = (int64_t) xr_value_str_len(&b);
        return xr_sort_core_compare_default(a, b, a_data, a_len, b_data, b_len);
    }
    return xr_sort_core_compare_default(a, b, NULL, 0, NULL, 0);
}

static int xr_sort_with_comparator(void *ctx_ptr, XrValue va, XrValue vb) {
    XrSortCtx *ctx = (XrSortCtx *) ctx_ptr;
    if (ctx->comparator) {
        XrValue args[2] = {va, vb};
        return xr_sort_core_compare_result(xr_vm_call_closure(ctx->iso, ctx->comparator, args, 2));
    }
    return xr_array_sort_default_cmp(va, vb);
}

static void xr_array_typed_sort_with_comparator(struct XrVMRuntime *iso, XrArray *arr,
                                                struct XrClosure *cmp) {
    int n = arr->length;
    XrValue *boxed = (XrValue *) xr_malloc((size_t) n * sizeof(XrValue));
    if (!boxed)
        return;
    for (int i = 0; i < n; i++)
        boxed[i] = xr_array_get_element(arr, i);
    XrSortCtx ctx = {iso, cmp};
    xr_sort_core_values(boxed, n, xr_sort_with_comparator, &ctx);
    for (int i = 0; i < n; i++)
        xr_array_set_element(arr, i, boxed[i]);
    xr_free(boxed);
}

void xr_array_sort(struct XrVMRuntime *iso, XrArray *arr, struct XrClosure *comparator) {
    if (!arr || arr->length <= 1)
        return;

    if (arr->elem_type == XR_ELEM_ANY) {
        XrSortCtx ctx = {iso, comparator};
        xr_sort_core_values((XrValue *) arr->data, arr->length, xr_sort_with_comparator, &ctx);
        return;
    }

    if (comparator) {
        xr_array_typed_sort_with_comparator(iso, arr, comparator);
        return;
    }

    (void) xr_sort_core_typed(arr->data, arr->length, arr->elem_type);
}

typedef struct XrArrayJoinVMCtx {
    XrArray *arr;
} XrArrayJoinVMCtx;

static bool xr_array_join_vm_part(XrValue val, char *dst, size_t *len) {
    const char *data = NULL;
    size_t n = 0;
    char tmp[64];

    if (XR_IS_STRING(val)) {
        data = xr_value_str_data(&val);
        n = xr_value_str_len(&val);
    } else if (XR_IS_INT(val)) {
        int written = snprintf(tmp, sizeof(tmp), "%lld", (long long) XR_TO_INT(val));
        if (written < 0 || (size_t) written >= sizeof(tmp))
            return false;
        data = tmp;
        n = (size_t) written;
    } else if (XR_IS_FLOAT(val)) {
        int written = xr_format_float(tmp, sizeof(tmp), XR_TO_FLOAT(val));
        if (written < 0 || (size_t) written >= sizeof(tmp))
            return false;
        data = tmp;
        n = (size_t) written;
    } else if (XR_IS_BOOL(val)) {
        data = XR_TO_BOOL(val) ? "true" : "false";
        n = XR_TO_BOOL(val) ? 4 : 5;
    } else if (XR_IS_NULL(val)) {
        data = "null";
        n = 4;
    } else {
        data = "[object]";
        n = 8;
    }

    if (dst && n > 0)
        memcpy(dst, data, n);
    if (len)
        *len = n;
    return true;
}

static bool xr_array_join_vm_element(void *ctx, int64_t index, char *dst, size_t *len) {
    XrArrayJoinVMCtx *join_ctx = (XrArrayJoinVMCtx *) ctx;
    if (!join_ctx || !join_ctx->arr || index < 0 || index > INT32_MAX)
        return false;
    return xr_array_join_vm_part(xr_array_get_element(join_ctx->arr, (int32_t) index), dst, len);
}

struct XrString *xr_array_join(struct XrVMRuntime *iso, XrArray *arr, struct XrString *delimiter) {
    if (arr == NULL || arr->length == 0)
        return xr_string_intern(iso, "", 0, 0);

    const char *sep = delimiter ? delimiter->data : NULL;
    size_t sep_len = delimiter ? delimiter->length : 0;
    XrArrayJoinVMCtx ctx = {arr};
    size_t total = 0;
    if (!xr_array_core_join_total(arr->length, sep_len, xr_array_join_vm_element, &ctx, &total))
        return NULL;

    if (total == 0)
        return xr_string_intern(iso, "", 0, 0);

    char stack_buf[256];
    char *buf = total < sizeof(stack_buf) ? stack_buf : (char *) xr_malloc(total + 1);
    if (!buf)
        return NULL;

    size_t written = 0;
    bool ok = xr_array_core_join_write(buf, total + 1, arr->length, sep, sep_len,
                                       xr_array_join_vm_element, &ctx, &written);
    XrString *result = ok ? xr_string_intern(iso, buf, written, 0) : NULL;
    if (buf != stack_buf)
        xr_free(buf);
    return result;
}

struct XrString *xr_array_to_string(struct XrVMRuntime *iso, XrArray *arr) {
    if (!arr || arr->length == 0)
        return xr_string_intern(iso, "", 0, 0);
    return xr_string_intern(iso, (const char *) arr->data, arr->length, 0);
}
