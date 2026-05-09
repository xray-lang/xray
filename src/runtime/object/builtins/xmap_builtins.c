/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmap_builtins.c - Map builtin functions
 *
 * KEY CONCEPT:
 *   Global Map constructor and static methods.
 */

#include "xmap_builtins.h"
#include "xchecks.h"
#include "xarray.h"
#include "xmap.h"
#include "xvalue.h"
#include "xisolate_api.h"
#include "xgc.h"

// Map() - create empty map
XrValue xr_builtin_map_construct(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    XR_DCHECK(isolate != NULL, "map_construct: NULL isolate");
    (void) self;
    (void) args;
    if (argc != 0)
        return xr_null();
    XrMap *map = xr_map_new(xr_current_coro(isolate));
    return xr_value_from_map(map);
}

// Map.from(entries) or Map.from(keys, values)
XrValue xr_builtin_map_from(XrayIsolate *isolate, XrValue self, XrValue *args, int argc) {
    (void) self;
    if (argc == 1) {
        // Map.from(entries) - array of [key, value] pairs
        if (!XR_IS_ARRAY(args[0]))
            return xr_null();

        XrArray *entries = XR_TO_ARRAY(args[0]);
        XrMap *map = xr_map_new(xr_current_coro(isolate));

        XrValue *edata = (XrValue *) entries->data;
        for (int i = 0; i < entries->length; i++) {
            if (!XR_IS_ARRAY(edata[i]))
                return xr_null();

            XrArray *entry = XR_TO_ARRAY(edata[i]);
            if (entry->length != 2)
                return xr_null();

            XrValue *pdata = (XrValue *) entry->data;
            xr_map_set(map, pdata[0], pdata[1]);
        }

        return xr_value_from_map(map);
    } else if (argc == 2) {
        // Map.from(keys, values)
        if (!XR_IS_ARRAY(args[0]) || !XR_IS_ARRAY(args[1]))
            return xr_null();

        XrArray *keys = XR_TO_ARRAY(args[0]);
        XrArray *values = XR_TO_ARRAY(args[1]);

        int size = keys->length < values->length ? keys->length : values->length;
        XrMap *map = xr_map_new(xr_current_coro(isolate));

        for (int i = 0; i < size; i++) {
            xr_map_set(map, ((XrValue *) keys->data)[i], ((XrValue *) values->data)[i]);
        }

        return xr_value_from_map(map);
    }
    return xr_null();
}
