/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_builtins.c - Array built-in functions implementation
 *
 * KEY CONCEPT:
 *   Global Array constructor and static methods (from, range, withCapacity).
 */

#include "xarray_builtins.h"
#include "xchecks.h"
#include "xisolate_api.h"
#include "xgc.h"
#include "xarray.h"
#include "xstring.h"
#include "xset.h"
#include "xmap.h"
#include "xvalue.h"
#include <stdio.h>

// Array() - empty array
// Array(n) - length n, filled with null
// Array(n, value) - length n, filled with value
XrValue xr_builtin_array_construct(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "array_construct: NULL isolate");
    if (nargs == 0) {
        // Array() - empty array
        XrArray *arr = xr_array_new(xr_current_coro(isolate));
        return xr_value_from_array(arr);
    } else if (nargs == 1) {
        // Array(n) - length n, filled with null
        if (!XR_IS_INT(args[0])) {
            xr_runtime_error(isolate, "Array(n): n must be integer\n");
            return xr_null();
        }

        int n = (int) XR_TO_INT(args[0]);
        if (n < 0) {
            xr_runtime_error(isolate, "Array(n): n must be >= 0\n");
            return xr_null();
        }

        XrArray *arr = xr_array_with_capacity(xr_current_coro(isolate), n);
        XrValue *adata = (XrValue *) arr->data;
        for (int i = 0; i < n; i++) {
            adata[i] = xr_null();
        }
        arr->length = n;

        return xr_value_from_array(arr);
    } else if (nargs == 2) {
        // Array(n, value) - length n, filled with value
        if (!XR_IS_INT(args[0])) {
            xr_runtime_error(isolate, "Array(n, value): n must be integer\n");
            return xr_null();
        }

        int n = (int) XR_TO_INT(args[0]);
        if (n < 0) {
            xr_runtime_error(isolate, "Array(n, value): n must be >= 0\n");
            return xr_null();
        }

        XrValue value = args[1];

        XrArray *arr = xr_array_with_capacity(xr_current_coro(isolate), n);
        XrValue *adata = (XrValue *) arr->data;
        for (int i = 0; i < n; i++) {
            adata[i] = value;
        }
        arr->length = n;

        return xr_value_from_array(arr);
    } else {
        xr_runtime_error(isolate, "Array() expects 0, 1, or 2 arguments\n");
        return xr_null();
    }
}

// Array.from(iterable) - create array from iterable or string
XrValue xr_builtin_array_from(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "array_from: NULL isolate");
    if (nargs != 1) {
        xr_runtime_error(isolate, "Array.from() expects 1 argument\n");
        return xr_null();
    }

    XrValue source = args[0];

    // From string
    if (XR_IS_STRING(source)) {
        XrString *str = XR_TO_STRING(source);
        XrArray *arr = xr_array_with_capacity(xr_current_coro(isolate), (int) str->length);

        for (size_t i = 0; i < str->length; i++) {
            char ch[2] = {str->data[i], '\0'};
            XrString *char_str = xr_string_intern(isolate, ch, 1, 0);
            ((XrValue *) arr->data)[i] = xr_string_value(char_str);
        }
        arr->length = (int) str->length;

        return xr_value_from_array(arr);
    }

    // From array (copy)
    if (XR_IS_ARRAY(source)) {
        XrArray *src = XR_TO_ARRAY(source);
        XrArray *arr = xr_array_with_capacity(xr_current_coro(isolate), src->length);

        XrValue *dst = (XrValue *) arr->data;
        XrValue *srcd = (XrValue *) src->data;
        for (int i = 0; i < src->length; i++) {
            dst[i] = srcd[i];
        }
        arr->length = src->length;

        return xr_value_from_array(arr);
    }

    // From Set
    if (XR_IS_SET(source)) {
        XrSet *set = XR_TO_SET(source);
        XrArray *arr = xr_array_with_capacity(xr_current_coro(isolate), set->count);

        if (set->entries) {
            for (uint32_t i = 0; i < set->capacity; i++) {
                XrSetEntry *entry = &set->entries[i];
                // Check if entry is valid (state >= 0x80)
                if (entry->state >= 0x80) {
                    ((XrValue *) arr->data)[arr->length++] = entry->value;
                }
            }
        }

        return xr_value_from_array(arr);
    }

    // From Map (returns entries array)
    if (XR_IS_MAP(source)) {
        XrMap *map = XR_TO_MAP(source);
        XrArray *arr = xr_map_entries(xr_current_coro(isolate), map);
        return xr_value_from_array(arr);
    }

    xr_runtime_error(isolate, "Array.from(): unsupported type\n");
    return xr_null();
}

// Array.range(start, end) - create array [start, start+1, ..., end]
XrValue xr_builtin_array_range(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "array_range: NULL isolate");
    if (nargs != 2) {
        xr_runtime_error(isolate, "Array.range() expects 2 arguments\n");
        return xr_null();
    }

    if (!XR_IS_INT(args[0]) || !XR_IS_INT(args[1])) {
        xr_runtime_error(isolate, "Array.range(start, end): both must be integers\n");
        return xr_null();
    }

    xr_Integer start = XR_TO_INT(args[0]);
    xr_Integer end = XR_TO_INT(args[1]);

    if (start > end) {
        xr_runtime_error(isolate, "Array.range(): start must be <= end\n");
        return xr_null();
    }

    int size = (int) (end - start + 1);
    XrArray *arr = xr_array_with_capacity(xr_current_coro(isolate), size);

    for (int i = 0; i < size; i++) {
        ((XrValue *) arr->data)[i] = xr_int(start + i);
    }
    arr->length = size;

    return xr_value_from_array(arr);
}

// Array.withCapacity(n) - capacity n, length 0 (performance optimization)
XrValue xr_builtin_array_with_capacity(XrayIsolate *isolate, XrValue self, XrValue *args, int nargs) {
    (void) self;
    XR_DCHECK(isolate != NULL, "array_with_capacity: NULL isolate");
    if (nargs != 1) {
        xr_runtime_error(isolate, "Array.withCapacity() expects 1 argument\n");
        return xr_null();
    }

    if (!XR_IS_INT(args[0])) {
        xr_runtime_error(isolate, "Array.withCapacity(n): n must be integer\n");
        return xr_null();
    }

    int capacity = (int) XR_TO_INT(args[0]);
    if (capacity < 0) {
        xr_runtime_error(isolate, "Array.withCapacity(n): n must be >= 0\n");
        return xr_null();
    }

    XrArray *arr = xr_array_with_capacity(xr_current_coro(isolate), capacity);
    return xr_value_from_array(arr);
}
