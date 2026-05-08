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
#include "xstringbuilder_builtins.h"
#include "xslice_builtins.h"
#include "../value/xtype_names.h"

/* Forward declarations: register functions live in *_methods.c files.
 * We call them here to unify core->xxxClass with native_type_classes[]. */
extern void xr_array_register_native_type(XrayIsolate *);
extern void xr_map_register_native_type(XrayIsolate *);
extern void xr_set_register_native_type(XrayIsolate *);
extern void xr_string_register_native_type(XrayIsolate *);
extern void xr_int_register_native_type(XrayIsolate *);
extern void xr_float_register_native_type(XrayIsolate *);
extern void xr_bool_register_native_type(XrayIsolate *);
extern void xr_bigint_register_native_type(XrayIsolate *);
extern void xr_json_register_native_type(XrayIsolate *);
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

    // Step 2: Create builtin type classes.
    // Collection and value types use xr_xxx_register_native_type() which
    // builds a single XrClass with constructor + static methods + instance
    // methods and stores it in native_type_classes[].  We then point
    // core->xxxClass at the same object so callers that use either
    // pathway see the same class.
    xr_string_register_native_type(X);
    X->core->stringClass = xr_isolate_get_native_type_class(X, XR_TSTRING);
    xr_array_register_native_type(X);
    X->core->arrayClass = xr_isolate_get_native_type_class(X, XR_TARRAY);
    xr_map_register_native_type(X);
    X->core->mapClass = xr_isolate_get_native_type_class(X, XR_TMAP);
    xr_set_register_native_type(X);
    X->core->setClass = xr_isolate_get_native_type_class(X, XR_TSET);
    xr_json_register_native_type(X);

    xr_int_register_native_type(X);
    X->core->intClass = xr_isolate_get_native_type_class(X, XR_TINT);
    xr_float_register_native_type(X);
    X->core->floatClass = xr_isolate_get_native_type_class(X, XR_TFLOAT);
    xr_bool_register_native_type(X);
    X->core->boolClass = xr_isolate_get_native_type_class(X, XR_TBOOL);
    X->core->nullClass = xr_class_new(X, TYPE_NAME_NULL, X->core->objectClass);
    xr_bigint_register_native_type(X);
    X->core->bigintClass = xr_isolate_get_native_type_class(X, XR_TBIGINT);

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
