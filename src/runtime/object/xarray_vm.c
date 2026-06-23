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
#include "../../shared/xr_sort_core.h"
#include "../../vm/xvm_string.h"
#include "../xvm_call.h"
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

struct XrString *xr_array_join(struct XrVMRuntime *iso, XrArray *arr, struct XrString *delimiter) {
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

struct XrString *xr_array_to_string(struct XrVMRuntime *iso, XrArray *arr) {
    if (!arr || arr->length == 0)
        return xr_string_intern(iso, "", 0, 0);
    return xr_string_intern(iso, (const char *) arr->data, arr->length, 0);
}
