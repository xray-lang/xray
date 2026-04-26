/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_instance_methods.c - Map instance methods
 *
 * KEY CONCEPT:
 *   Map instance methods: set, get, has, delete, clear, increment.
 */

#include "xmap_instance_methods.h"
#include "../../base/xchecks.h"
#include "xmap.h"
#include "../value/xvalue.h"
#include "../class/xclass.h"
#include "../class/xmethod.h"
#include "../symbol/xsymbol_table.h"
#include "../xisolate_api.h"
#include "../class/xclass_system.h"
#include <stdio.h>

// Map.prototype.set(key, value) - returns this for chaining
XrValue xr_map_method_set(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 3)
        return xr_null();

    XrValue this_val = args[0];
    if (!XR_IS_MAP(this_val))
        return xr_null();

    XrMap *map = XR_TO_MAP(this_val);
    xr_map_set(map, args[1], args[2]);
    return this_val;
}

// Map.prototype.get(key) - returns null if not found
XrValue xr_map_method_get(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2)
        return xr_null();

    XrValue this_val = args[0];
    if (!XR_IS_MAP(this_val))
        return xr_null();

    XrMap *map = XR_TO_MAP(this_val);
    bool found = false;
    XrValue result = xr_map_get(map, args[1], &found);
    return found ? result : xr_null();
}

// Map.prototype.has(key)
XrValue xr_map_method_has(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2)
        return xr_bool(false);

    XrValue this_val = args[0];
    if (!XR_IS_MAP(this_val))
        return xr_bool(false);

    XrMap *map = XR_TO_MAP(this_val);
    return xr_bool(xr_map_has(map, args[1]));
}

// Map.prototype.delete(key) - returns true if deleted
XrValue xr_map_method_delete(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2)
        return xr_bool(false);

    XrValue this_val = args[0];
    if (!XR_IS_MAP(this_val))
        return xr_bool(false);

    XrMap *map = XR_TO_MAP(this_val);
    return xr_bool(xr_map_delete(map, args[1]));
}

// Map.prototype.clear()
XrValue xr_map_method_clear(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 1)
        return xr_null();

    XrValue this_val = args[0];
    if (!XR_IS_MAP(this_val))
        return xr_null();

    xr_map_clear(XR_TO_MAP(this_val));
    return xr_null();
}

// Map.increment(key) - optimized counter: set to 1 if not exists, else add 1
XrValue xr_map_method_increment(XrayIsolate *isolate, XrValue *args, int nargs) {
    (void) isolate;
    if (nargs < 2)
        return xr_null();

    XrValue this_val = args[0];
    if (!XR_IS_MAP(this_val))
        return xr_null();

    XrMap *map = XR_TO_MAP(this_val);
    XrValue key = args[1];
    bool found = false;
    XrValue old_val = xr_map_get(map, key, &found);

    if (found && XR_IS_INT(old_val)) {
        xr_map_set(map, key, xr_int(XR_TO_INT(old_val) + 1));
    } else {
        xr_map_set(map, key, xr_int(1));
    }
    return xr_null();
}

// Note: Map instance methods are now registered via XrClassBuilder in xr_map_create_class()
