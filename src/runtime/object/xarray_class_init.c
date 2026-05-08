/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xarray_class_init.c - Array class initialization
 *
 * KEY CONCEPT:
 *   Create Array class with constructor using XrClassBuilder.
 */

#include "xarray_class_init.h"
#include "../../base/xchecks.h"
#include "builtins/xarray_builtins.h"
#include "../class/xclass.h"
#include "../class/xclass_builder.h"
#include "../xisolate_api.h"
#include <stdio.h>

// Create Array class with all methods using XrClassBuilder
XrClass *xr_array_create_class(XrayIsolate *X, XrClass *objectClass) {
    XrClassBuilder *builder = xr_class_builder_new(X, "Array", objectClass);
    if (!builder) {
        fprintf(stderr, "[Array] ERROR: Failed to create class builder\n");
        return NULL;
    }

    // Static constructor
    xr_class_builder_add_static_method(builder, XR_KEYWORD_CONSTRUCTOR,
                                       xr_builtin_array_construct, 0, 0);

    return xr_class_builder_finalize(builder);
}
