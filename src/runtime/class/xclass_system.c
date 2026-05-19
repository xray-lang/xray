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
#include "xclass_builder.h" /* Process class still uses builder */
#include "../../base/xchecks.h"
#include "../xisolate_internal.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue.h"
#include "xstringbuilder_builtins.h"
#include "xenum.h"
#include "../value/xtype_names.h"
#include "../object/xexception.h"

/* Forward declarations: register functions live in *_methods.c files.
 * We call them here to unify core->xxxClass with native_type_classes[]. */
extern void xr_array_register_native_type(XrayIsolate *);
extern void xr_map_register_native_type(XrayIsolate *);
extern void xr_set_register_native_type(XrayIsolate *);
extern void xr_string_register_native_type(XrayIsolate *);
extern void xr_int_register_native_type(XrayIsolate *);
extern void xr_float_register_native_type(XrayIsolate *);
extern void xr_bool_register_native_type(XrayIsolate *);
extern void xr_bigint_register_class(XrayIsolate *);
extern void xr_json_register_instance_methods(XrayIsolate *);
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
    // Json instance methods: build a plain XrClass with instance methods
    // (iterator, toString, keys, values, has, etc.). The class is wired as
    // jsonRootClass->super so dynamic-layout instances find these methods
    // via normal class-chain lookup.
    xr_json_register_instance_methods(X);

    // Dynamic-layout root class for Json: open hidden-class chain, 8 in-object
    // slots (7 logical + 1 overflow pointer reservation). All Json objects
    // start at this class and transition as fields are added.
    X->core->jsonRootClass = xr_class_new_dynamic_root(X, "Json", 8, false);
    X->core->jsonRootClass->super = X->core->jsonInstanceMethodClass;

    xr_int_register_native_type(X);
    X->core->intClass = xr_isolate_get_native_type_class(X, XR_TINT);
    xr_float_register_native_type(X);
    X->core->floatClass = xr_isolate_get_native_type_class(X, XR_TFLOAT);
    xr_bool_register_native_type(X);
    X->core->boolClass = xr_isolate_get_native_type_class(X, XR_TBOOL);
    X->core->nullClass = xr_class_new(X, TYPE_NAME_NULL, X->core->objectClass);
    xr_isolate_set_native_type_class(X, XR_TNULL, X->core->nullClass);
    xr_bigint_register_class(X);

    X->core->functionClass = xr_class_new(X, TYPE_NAME_FUNCTION, X->core->objectClass);
    X->core->closureClass = xr_class_new(X, TYPE_NAME_CLOSURE, X->core->functionClass);
    X->core->upvalueClass = xr_class_new(X, TYPE_NAME_UPVALUE, X->core->objectClass);
    X->core->cfunctionClass = xr_class_new(X, TYPE_NAME_CFUNCTION, X->core->functionClass);

    X->core->enumClass = xr_class_new(X, CLASS_NAME_ENUM, X->core->objectClass);
    xr_class_mark_abstract(X->core->enumClass);

    // Internal classes for enum instances (not user-visible)
    {
        XrClassBuilder *b = xr_class_builder_new(X, "EnumValue", X->core->enumClass);
        xr_class_builder_set_native_body(b, xr_enum_value_native_body_desc());
        X->core->enumValueClass = xr_class_builder_finalize(b);
        X->core->enumValueClass->flags |= XR_CLASS_BUILTIN | XR_CLASS_ENUM_VALUE;
    }
    {
        XrClassBuilder *b = xr_class_builder_new(X, "EnumType", X->core->objectClass);
        xr_class_builder_set_native_body(b, xr_enum_type_native_body_desc());
        X->core->enumTypeClass = xr_class_builder_finalize(b);
        X->core->enumTypeClass->flags |= XR_CLASS_BUILTIN | XR_CLASS_ENUM_TYPE;
    }

    xr_stringbuilder_register_class(X);

    // Process class with fields
    {
        XrClassBuilder *builder = xr_class_builder_new(X, "Process", X->core->objectClass);
        xr_class_builder_add_field(builder, "file", 0);
        xr_class_builder_add_field(builder, "args", 0);
        xr_class_builder_add_field(builder, "dir", 0);
        X->core->processClass = xr_class_builder_finalize(builder);
    }

    // Exception class with 5 fields + primitive constructor + toString.
    // Registered here (not in stdlib prelude) because VM throw paths
    // need a valid core->exceptionClass before any user code runs;
    // bootstrap errors (OOM, type mismatch on early init) must succeed.
    xr_register_exception_class(X);

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
