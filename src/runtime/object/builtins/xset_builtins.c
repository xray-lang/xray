/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset_builtins.c - Set builtin functions
 *
 * KEY CONCEPT:
 *   Global Set constructor and static methods.
 */

#include "xchecks.h"
#include "xgc.h"
#include "xset_builtins.h"
#include "xisolate_api.h"
#include "xset.h"
#include "xarray.h"
#include "xstring.h"
#include "xvalue.h"
#include <stdio.h>

// Set() or Set(array) - create empty set or from array
XrValue xr_builtin_set_construct(XrayIsolate *isolate, XrValue *args, int nargs) {
    XR_DCHECK(isolate != NULL, "set_construct: NULL isolate");
    if (nargs == 0) {
        XrSet *set = xr_set_new(xr_current_coro(isolate));
        return xr_value_from_set(set);
    }

    if (nargs == 1 && XR_IS_ARRAY(args[0])) {
        XrArray *arr = XR_TO_ARRAY(args[0]);
        XrSet *set = xr_set_from_array(xr_current_coro(isolate), arr);
        return xr_value_from_set(set);
    }

    xr_runtime_error(isolate, "Set() expects 0 arguments or Set(array)\n");
    return xr_null();
}

// Set.from(iterable) - create Set from array or string (auto dedup)
XrValue xr_builtin_set_from(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs != 1) {
        xr_runtime_error(isolate, "Set.from() expects 1 argument\n");
        return xr_null();
    }

    XrValue source = args[0];

    // From string (unique chars)
    if (XR_IS_STRING(source)) {
        XrString *str = XR_TO_STRING(source);
        XrSet *set = xr_set_new(xr_current_coro(isolate));

        for (size_t i = 0; i < str->length; i++) {
            char ch[2] = {str->data[i], '\0'};
            XrString *char_str = xr_string_intern(isolate, ch, 1, 0);
            xr_set_add(set, xr_string_value(char_str));
        }

        return xr_value_from_set(set);
    }

    // From array (dedup)
    if (XR_IS_ARRAY(source)) {
        XrArray *arr = XR_TO_ARRAY(source);
        XrSet *set = xr_set_from_array(xr_current_coro(isolate), arr);
        return xr_value_from_set(set);
    }

    // From Set (copy)
    if (XR_IS_SET(source)) {
        XrSet *src = XR_TO_SET(source);
        XrSet *set = xr_set_new(xr_current_coro(isolate));

        if (src->entries) {
            for (uint32_t i = 0; i < src->capacity; i++) {
                XrSetEntry *entry = &src->entries[i];
                if (entry->state >= 0x80) {
                    xr_set_add(set, entry->value);
                }
            }
        }

        return xr_value_from_set(set);
    }

    xr_runtime_error(isolate, "Set.from(): unsupported type\n");
    return xr_null();
}

// Set.range(start, end) - create Set with integer range
XrValue xr_builtin_set_range(XrayIsolate *isolate, XrValue *args, int nargs) {
    if (nargs != 2) {
        xr_runtime_error(isolate, "Set.range() expects 2 arguments\n");
        return xr_null();
    }

    if (!XR_IS_INT(args[0]) || !XR_IS_INT(args[1])) {
        xr_runtime_error(isolate, "Set.range(start, end): both must be integers\n");
        return xr_null();
    }

    xr_Integer start = XR_TO_INT(args[0]);
    xr_Integer end = XR_TO_INT(args[1]);

    if (start > end) {
        xr_runtime_error(isolate, "Set.range(): start must be <= end\n");
        return xr_null();
    }

    XrSet *set = xr_set_new(xr_current_coro(isolate));

    for (xr_Integer i = start; i <= end; i++) {
        xr_set_add(set, xr_int(i));
    }

    return xr_value_from_set(set);
}
