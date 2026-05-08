/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_system.c - Core class system initialization
 *
 * KEY CONCEPT:
 *   Initialize builtin types: Object (root), containers, primitives, functions, Enum.
 */

#include "xclass_system.h"
#include "../../base/xlog.h"
#include "xclass.h"
#include "xclass_builder.h"
#include "../../base/xchecks.h"
#include "../xisolate_internal.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue.h"
#include "../../base/xhashmap.h"
#include "xreflect_registry.h"
#include "xmethod.h"
#include "xmap_builtins.h"
#include "../object/xarray_class_init.h"
#include "../object/xset_class_init.h"
#include "xstringbuilder_builtins.h"
#include "xslice_builtins.h"
#include "../value/xtype_names.h"
#include <stdio.h>
#include <string.h>

void xr_core_init(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "xr_core_init: NULL isolate");
    X->core = (XrayCoreClasses *) xr_malloc(sizeof(XrayCoreClasses));
    if (!X->core) {
        xr_log_warning("class", "Fatal: Failed to allocate core classes");
        return;
    }
    memset(X->core, 0, sizeof(XrayCoreClasses));

    // Step 1: Create Object root class
    X->core->objectClass = xr_class_new(X, CLASS_NAME_OBJECT, NULL);

    // Step 2: Create builtin type classes (all inherit from Object)
    X->core->stringClass = xr_class_new(X, TYPE_NAME_STRING, X->core->objectClass);
    X->core->arrayClass = xr_array_create_class(X, X->core->objectClass);
    X->core->mapClass = xr_map_create_class(X, X->core->objectClass);
    X->core->setClass = xr_set_create_class(X, X->core->objectClass);

    X->core->intClass = xr_class_new(X, TYPE_NAME_INT, X->core->objectClass);
    X->core->floatClass = xr_class_new(X, TYPE_NAME_FLOAT, X->core->objectClass);
    X->core->boolClass = xr_class_new(X, TYPE_NAME_BOOL, X->core->objectClass);
    X->core->nullClass = xr_class_new(X, TYPE_NAME_NULL, X->core->objectClass);
    X->core->bigintClass = xr_class_new(X, TYPE_NAME_BIGINT, X->core->objectClass);

    X->core->functionClass = xr_class_new(X, TYPE_NAME_FUNCTION, X->core->objectClass);
    X->core->closureClass = xr_class_new(X, TYPE_NAME_CLOSURE, X->core->functionClass);
    X->core->upvalueClass = xr_class_new(X, TYPE_NAME_UPVALUE, X->core->objectClass);
    X->core->cfunctionClass = xr_class_new(X, TYPE_NAME_CFUNCTION, X->core->functionClass);

    X->core->enumClass = xr_class_new(X, CLASS_NAME_ENUM, X->core->objectClass);
    xr_class_mark_abstract(X->core->enumClass);

    X->core->stringBuilderClass = xr_stringbuilder_create_class(X, X->core->objectClass);
    xr_isolate_set_native_type_class(X, XR_TSTRINGBUILDER, X->core->stringBuilderClass);
    X->core->arraySliceClass = xr_array_slice_create_class(X, X->core->objectClass);
    xr_isolate_set_native_type_class(X, XR_TARRAY_SLICE, X->core->arraySliceClass);

    // Process class with fields
    {
        XrClassBuilder *builder = xr_class_builder_new(X, "Process", X->core->objectClass);
        xr_class_builder_add_field(builder, "file", 0);
        xr_class_builder_add_field(builder, "args", 0);
        xr_class_builder_add_field(builder, "dir", 0);
        X->core->processClass = xr_class_builder_finalize(builder);
    }

    // All classes above were created through xr_class_new / a builder,
    // and xr_class_builder_finalize already registers every finished
    // class with the type registry. The explicit registration loop that
    // used to live here was pure duplication -- kept around only because
    // the old lazy-reflection design did not guarantee registration at
    // construction time.
}

void xr_core_free(XrayIsolate *X) {
    if (!X || !X->core)
        return;

    // Core classes are GC-managed, just free the container
    xr_free(X->core);
    X->core = NULL;
}
