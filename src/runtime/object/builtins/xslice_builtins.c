/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xslice_builtins.c - Slice type builtin methods
 *
 * KEY CONCEPT:
 *   Builtin methods for ArraySlice/ByteSlice types.
 *   Slices are views into underlying arrays without copying data.
 */

#include "xchecks.h"
#include "xgc.h"
#include "xslice_builtins.h"
#include "xclass_system.h"
#include "xslice.h"
#include "xarray.h"
#include "xstring.h"
#include "xclass.h"
#include "xmethod.h"
#include "xsymbol_table.h"
#include "xisolate_api.h"
#include "xvm_call.h"
#include <stdio.h>

/* ========== ArraySlice Methods ========== */

// arraySlice.length
XrValue xr_builtin_array_slice_length(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    (void) isolate;
    (void) args;

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: length: not an ArraySlice\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    return xr_int(xr_array_slice_length(slice));
}

// arraySlice.get(index)
XrValue xr_builtin_array_slice_get(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    (void) isolate;

    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.get() expects 1 argument\n");
        return xr_null();
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: get: not an ArraySlice\n");
        return xr_null();
    }

    if (!XR_IS_INT(args[0])) {
        fprintf(stderr, "Error: ArraySlice.get(index): index must be integer\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    int index = (int) XR_TO_INT(args[0]);

    return xr_array_slice_get(slice, index);
}

// arraySlice.set(index, value)
XrValue xr_builtin_array_slice_set(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    (void) isolate;

    if (argc < 2) {
        fprintf(stderr, "Error: ArraySlice.set() expects 2 arguments\n");
        return xr_null();
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: set: not an ArraySlice\n");
        return xr_null();
    }

    if (!XR_IS_INT(args[0])) {
        fprintf(stderr, "Error: ArraySlice.set(index, value): index must be integer\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    int index = (int) XR_TO_INT(args[0]);

    xr_array_slice_set(slice, index, args[1]);
    return xr_null();
}

// arraySlice.toArray() - convert to independent array (copy)
XrValue xr_builtin_array_slice_to_array(XrayIsolate *isolate, XrValue self, XrValue *args,
                                        int argc) {
    XR_DCHECK(isolate != NULL, "slice_to_array: NULL isolate");
    (void) argc;
    (void) args;

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: toArray: not an ArraySlice\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrArray *arr = xr_array_slice_to_array(xr_current_coro(isolate), slice);
    return XR_FROM_PTR(arr);
}

// arraySlice.indexOf(value)
XrValue xr_builtin_array_slice_index_of(XrayIsolate *isolate, XrValue self, XrValue *args,
                                        int argc) {
    (void) isolate;

    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.indexOf() expects 1 argument\n");
        return xr_int(-1);
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: indexOf: not an ArraySlice\n");
        return xr_int(-1);
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    int32_t len = xr_array_slice_length(slice);
    XrValue target = args[0];

    for (int32_t i = 0; i < len; i++) {
        XrValue elem = xr_array_slice_get(slice, i);
        if (xr_value_deep_eq(elem, target)) {
            return xr_int(i);
        }
    }
    return xr_int(-1);
}

// arraySlice.contains(value)
XrValue xr_builtin_array_slice_contains(XrayIsolate *isolate, XrValue self, XrValue *args,
                                        int argc) {
    (void) isolate;

    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.contains() expects 1 argument\n");
        return xr_bool(0);
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: contains: not an ArraySlice\n");
        return xr_bool(0);
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    int32_t len = xr_array_slice_length(slice);
    XrValue target = args[0];

    for (int32_t i = 0; i < len; i++) {
        XrValue elem = xr_array_slice_get(slice, i);
        if (xr_value_deep_eq(elem, target)) {
            return xr_bool(1);
        }
    }
    return xr_bool(0);
}

// arraySlice.first()
XrValue xr_builtin_array_slice_first(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    (void) isolate;
    (void) args;

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: first: not an ArraySlice\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    if (xr_array_slice_length(slice) == 0) {
        return xr_null();
    }
    return xr_array_slice_get(slice, 0);
}

// arraySlice.last()
XrValue xr_builtin_array_slice_last(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    (void) isolate;
    (void) argc;
    (void) args;
    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: last: not an ArraySlice\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    int32_t len = xr_array_slice_length(slice);
    if (len == 0) {
        return xr_null();
    }
    return xr_array_slice_get(slice, len - 1);
}

// arraySlice.forEach(callback) - callback(element, index) -> void
XrValue xr_builtin_array_slice_for_each(XrayIsolate *isolate, XrValue self, XrValue *args,
                                        int argc) {
    XR_DCHECK(isolate != NULL, "slice_for_each: NULL isolate");
    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.forEach() expects 1 argument (callback)\n");
        return xr_null();
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: forEach: not an ArraySlice\n");
        return xr_null();
    }

    // Check if callback is a closure
    if (!XR_IS_FUNCTION(args[0])) {
        fprintf(stderr, "Error: forEach: callback must be a function\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrClosure *callback = xr_value_to_closure(args[0]);
    int32_t len = xr_array_slice_length(slice);

    // Iterate and call callback
    XrValue cb_args[2];
    for (int32_t i = 0; i < len; i++) {
        cb_args[0] = xr_array_slice_get(slice, i);  // element
        cb_args[1] = xr_int(i);                     // index
        xr_vm_call_closure(isolate, callback, cb_args, 2);
    }

    return xr_null();
}

// arraySlice.map(callback) - callback(element, index) -> newElement
XrValue xr_builtin_array_slice_map(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "slice_map: NULL isolate");
    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.map() expects 1 argument (callback)\n");
        return xr_null();
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: map: not an ArraySlice\n");
        return xr_null();
    }

    if (!XR_IS_FUNCTION(args[0])) {
        fprintf(stderr, "Error: map: callback must be a function\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrClosure *callback = xr_value_to_closure(args[0]);
    int32_t len = xr_array_slice_length(slice);

    // Create result array
    XrArray *result = xr_array_new(xr_current_coro(isolate));
    if (!result) {
        fprintf(stderr, "Error: map: failed to create result array\n");
        return xr_null();
    }

    // Iterate, call callback, collect results
    XrValue cb_args[2];
    for (int32_t i = 0; i < len; i++) {
        cb_args[0] = xr_array_slice_get(slice, i);  // element
        cb_args[1] = xr_int(i);                     // index
        XrValue mapped = xr_vm_call_closure(isolate, callback, cb_args, 2);
        xr_array_push(result, mapped);
    }

    return XR_FROM_PTR(result);
}

// arraySlice.filter(callback) - callback(element, index) -> bool
XrValue xr_builtin_array_slice_filter(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "slice_filter: NULL isolate");
    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.filter() expects 1 argument (callback)\n");
        return xr_null();
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: filter: not an ArraySlice\n");
        return xr_null();
    }

    if (!XR_IS_FUNCTION(args[0])) {
        fprintf(stderr, "Error: filter: callback must be a function\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrClosure *callback = xr_value_to_closure(args[0]);
    int32_t len = xr_array_slice_length(slice);

    // Create result array
    XrArray *result = xr_array_new(xr_current_coro(isolate));
    if (!result) {
        fprintf(stderr, "Error: filter: failed to create result array\n");
        return xr_null();
    }

    // Iterate, call callback, collect matching elements
    XrValue cb_args[2];
    for (int32_t i = 0; i < len; i++) {
        XrValue elem = xr_array_slice_get(slice, i);
        cb_args[0] = elem;       // element
        cb_args[1] = xr_int(i);  // index
        XrValue keep = xr_vm_call_closure(isolate, callback, cb_args, 2);
        if (XR_TO_BOOL(keep)) {
            xr_array_push(result, elem);
        }
    }

    return XR_FROM_PTR(result);
}

// arraySlice.reduce(callback, initial) - callback(acc, elem, idx) -> newAcc
XrValue xr_builtin_array_slice_reduce(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "slice_reduce: NULL isolate");
    if (argc < 2) {
        fprintf(stderr, "Error: ArraySlice.reduce() expects 2 arguments (callback, initial)\n");
        return xr_null();
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: reduce: not an ArraySlice\n");
        return xr_null();
    }

    if (!XR_IS_FUNCTION(args[0])) {
        fprintf(stderr, "Error: reduce: callback must be a function\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrClosure *callback = xr_value_to_closure(args[0]);
    XrValue accumulator = args[1];  // initial value
    int32_t len = xr_array_slice_length(slice);

    // Iterate, call callback, accumulate results
    XrValue cb_args[3];
    for (int32_t i = 0; i < len; i++) {
        cb_args[0] = accumulator;                   // accumulator
        cb_args[1] = xr_array_slice_get(slice, i);  // element
        cb_args[2] = xr_int(i);                     // index
        accumulator = xr_vm_call_closure(isolate, callback, cb_args, 3);
    }

    return accumulator;
}

// arraySlice.find(callback) - callback(element, index) -> bool
XrValue xr_builtin_array_slice_find(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "slice_find: NULL isolate");
    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.find() expects 1 argument (callback)\n");
        return xr_null();
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: find: not an ArraySlice\n");
        return xr_null();
    }

    if (!XR_IS_FUNCTION(args[0])) {
        fprintf(stderr, "Error: find: callback must be a function\n");
        return xr_null();
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrClosure *callback = xr_value_to_closure(args[0]);
    int32_t len = xr_array_slice_length(slice);

    // Search
    XrValue cb_args[2];
    for (int32_t i = 0; i < len; i++) {
        XrValue elem = xr_array_slice_get(slice, i);
        cb_args[0] = elem;       // element
        cb_args[1] = xr_int(i);  // index
        XrValue found = xr_vm_call_closure(isolate, callback, cb_args, 2);
        if (XR_TO_BOOL(found)) {
            return elem;
        }
    }

    return xr_null();
}

// arraySlice.every(callback) - callback(element, index) -> bool
XrValue xr_builtin_array_slice_every(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "slice_every: NULL isolate");
    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.every() expects 1 argument (callback)\n");
        return xr_bool(0);
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: every: not an ArraySlice\n");
        return xr_bool(0);
    }

    if (!XR_IS_FUNCTION(args[0])) {
        fprintf(stderr, "Error: every: callback must be a function\n");
        return xr_bool(0);
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrClosure *callback = xr_value_to_closure(args[0]);
    int32_t len = xr_array_slice_length(slice);

    XrValue cb_args[2];
    for (int32_t i = 0; i < len; i++) {
        cb_args[0] = xr_array_slice_get(slice, i);
        cb_args[1] = xr_int(i);  // index
        XrValue result = xr_vm_call_closure(isolate, callback, cb_args, 2);
        if (!XR_TO_BOOL(result)) {
            return xr_bool(0);
        }
    }

    return xr_bool(1);
}

// arraySlice.some(callback) - callback(element, index) -> bool
XrValue xr_builtin_array_slice_some(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "slice_some: NULL isolate");
    if (argc < 1) {
        fprintf(stderr, "Error: ArraySlice.some() expects 1 argument (callback)\n");
        return xr_bool(0);
    }

    if (!XR_IS_PTR(self) || XR_HEAP_TYPE(self) != XR_TARRAY_SLICE) {
        fprintf(stderr, "Error: some: not an ArraySlice\n");
        return xr_bool(0);
    }

    if (!XR_IS_FUNCTION(args[0])) {
        fprintf(stderr, "Error: some: callback must be a function\n");
        return xr_bool(0);
    }

    XrArraySlice *slice = (XrArraySlice *) XR_TO_PTR(self);
    XrClosure *callback = xr_value_to_closure(args[0]);
    int32_t len = xr_array_slice_length(slice);

    XrValue cb_args[2];
    for (int32_t i = 0; i < len; i++) {
        cb_args[0] = xr_array_slice_get(slice, i);
        cb_args[1] = xr_int(i);
        XrValue result = xr_vm_call_closure(isolate, callback, cb_args, 2);
        if (XR_TO_BOOL(result)) {
            return xr_bool(1);
        }
    }

    return xr_bool(0);
}

/* ========== Native Type Registration ========== */

#include "xnative_type.h"

void xr_array_slice_register_native_type(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "array_slice_register_native_type: NULL isolate");
    static const XrNativeMethod slice_methods[] = {
        /* Basic */
        {"length", xr_builtin_array_slice_length, 0},
        {"get", xr_builtin_array_slice_get, 1},
        {"set", xr_builtin_array_slice_set, 2},
        {"toArray", xr_builtin_array_slice_to_array, 0},
        {"indexOf", xr_builtin_array_slice_index_of, 1},
        {"contains", xr_builtin_array_slice_contains, 1},
        {"first", xr_builtin_array_slice_first, 0},
        {"last", xr_builtin_array_slice_last, 0},
        /* Higher-order */
        {"forEach", xr_builtin_array_slice_for_each, 1},
        {"map", xr_builtin_array_slice_map, 1},
        {"filter", xr_builtin_array_slice_filter, 1},
        {"reduce", xr_builtin_array_slice_reduce, 2},
        {"find", xr_builtin_array_slice_find, 1},
        {"every", xr_builtin_array_slice_every, 1},
        {"some", xr_builtin_array_slice_some, 1},
        {NULL, NULL, 0},
    };
    static const XrNativeTypeInfo slice_info = {
        .name = "ArraySlice",
        .gc_type = XR_TARRAY_SLICE,
        .methods = slice_methods,
        .getters = NULL,
        .static_methods = NULL,
    };
    xr_register_native_type(X, &slice_info);
}
