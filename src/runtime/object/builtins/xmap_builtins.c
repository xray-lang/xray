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
#include "xmap_instance_methods.h"
#include "xgc.h"
#include "xarray.h"  // Must be before xmap.h for XrArray definition
#include "xmap.h"
#include "xvalue.h"
#include "xclass.h"
#include "xclass_builder.h"
#include "xmethod.h"
#include "xsymbol_table.h"
#include "xisolate_api.h"
#include "xclass_system.h"
#include <stdio.h>

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

// Create Map class with all methods using XrClassBuilder
XrClass *xr_map_create_class(XrayIsolate *X, XrClass *objectClass) {
    XR_DCHECK(X != NULL, "map_create_class: NULL isolate");
    XrClassBuilder *builder = xr_class_builder_new(X, "Map", objectClass);
    if (!builder) {
        fprintf(stderr, "[Map] ERROR: Failed to create class builder\n");
        return NULL;
    }

    // Static constructor
    xr_class_builder_add_static_method(builder, XR_KEYWORD_CONSTRUCTOR,
                                       xr_builtin_map_construct, 0, 0);

    // Instance methods
    xr_class_builder_add_method(builder, "set", xr_map_method_set, 2, 0);
    xr_class_builder_add_method(builder, "get", xr_map_method_get, 1, 0);
    xr_class_builder_add_method(builder, "has", xr_map_method_has, 1, 0);
    xr_class_builder_add_method(builder, "delete", xr_map_method_delete, 1, 0);
    xr_class_builder_add_method(builder, "clear", xr_map_method_clear, 0, 0);
    xr_class_builder_add_method(builder, "increment", xr_map_method_increment, 1, 0);

    return xr_class_builder_finalize(builder);
}
