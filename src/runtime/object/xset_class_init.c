/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xset_class_init.c - Set class initialization
 *
 * KEY CONCEPT:
 *   Create Set class with constructor using XrClassBuilder.
 */

#include "xset_class_init.h"
#include "../../base/xchecks.h"
#include "builtins/xset_builtins.h"
#include "../class/xclass.h"
#include "../class/xclass_builder.h"
#include "../xisolate_api.h"
#include <stdio.h>

// Create Set class with all methods using XrClassBuilder
XrClass *xr_set_create_class(XrayIsolate *X, XrClass *objectClass) {
    XrClassBuilder *builder = xr_class_builder_new(X, "Set", objectClass);
    if (!builder) {
        fprintf(stderr, "[Set] ERROR: Failed to create class builder\n");
        return NULL;
    }

    // Static constructor
    xr_class_builder_add_static_method(builder, XR_KEYWORD_CONSTRUCTOR,
                                       xr_builtin_set_construct, 0, 0);

    return xr_class_builder_finalize(builder);
}
