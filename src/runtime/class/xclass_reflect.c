/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_reflect.c - Reflection API class creation and binding
 */

#include "xreflect_api.h"
#include "../../base/xchecks.h"
#include "xreflect_internal.h"
#include "xreflect_registry.h"
#include "../xisolate_api.h"
#include "xclass.h"
#include "xclass_builder.h"
#include "xclass_system.h"
#include <stdio.h>

/* ========== Reflect Class ========== */

static XrClass* create_reflect_class(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "create_reflect_class: NULL isolate");
    XrClassBuilder *builder = xr_class_builder_new(X, "Reflect", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder) return NULL;

    xr_class_builder_add_static_method(builder, "getType",
        (XrCFunctionPtr)xr_reflect_getType, 1, 0);
    xr_class_builder_add_static_method(builder, "getTypeByName",
        (XrCFunctionPtr)xr_reflect_getTypeByName, 1, 0);
    xr_class_builder_add_static_method(builder, "getAllTypes",
        (XrCFunctionPtr)xr_reflect_getAllTypes, 0, 0);
    xr_class_builder_add_static_method(builder, "isInstance",
        (XrCFunctionPtr)xr_reflect_isInstance, 2, 0);
    xr_class_builder_add_static_method(builder, "isInstanceOf",
        (XrCFunctionPtr)xr_reflect_isInstanceOf, 2, 0);
    xr_class_builder_add_static_method(builder, "elementType",
        (XrCFunctionPtr)xr_reflect_elementType, 1, 0);
    xr_class_builder_add_static_method(builder, "keyType",
        (XrCFunctionPtr)xr_reflect_keyType, 1, 0);
    xr_class_builder_add_static_method(builder, "valueType",
        (XrCFunctionPtr)xr_reflect_valueType, 1, 0);
    xr_class_builder_add_static_method(builder, "typeOf",
        (XrCFunctionPtr)xr_reflect_typeOf, 1, 0);
    xr_class_builder_add_static_method(builder, "fieldCount",
        (XrCFunctionPtr)xr_reflect_fieldCount, 1, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Type Class ========== */

static XrClass* create_type_class(XrayIsolate *X) {
    XrClassBuilder *builder = xr_class_builder_new(X, "Type", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder) return NULL;

    // Instance methods
    xr_class_builder_add_method(builder, "getFields",
        (XrCFunctionPtr)xr_type_getFields, 0, 0);
    xr_class_builder_add_method(builder, "getDeclaredFields",
        (XrCFunctionPtr)xr_type_getDeclaredFields, 0, 0);
    xr_class_builder_add_method(builder, "getField",
        (XrCFunctionPtr)xr_type_getField, 1, 0);
    xr_class_builder_add_method(builder, "getMethods",
        (XrCFunctionPtr)xr_type_getMethods, 0, 0);
    xr_class_builder_add_method(builder, "getDeclaredMethods",
        (XrCFunctionPtr)xr_type_getDeclaredMethods, 0, 0);
    xr_class_builder_add_method(builder, "getMethod",
        (XrCFunctionPtr)xr_type_getMethod, 1, 0);
    xr_class_builder_add_method(builder, "getConstructor",
        (XrCFunctionPtr)xr_type_getConstructor, 0, 0);
    xr_class_builder_add_method(builder, "newInstance",
        (XrCFunctionPtr)xr_type_newInstance, 0, 0);
    xr_class_builder_add_method(builder, "newInstanceWith",
        (XrCFunctionPtr)xr_type_newInstanceWith, 1, 0);
    xr_class_builder_add_method(builder, "isSubtypeOf",
        (XrCFunctionPtr)xr_type_isSubtypeOf, 1, 0);
    xr_class_builder_add_method(builder, "isAssignableFrom",
        (XrCFunctionPtr)xr_type_isAssignableFrom, 1, 0);
    xr_class_builder_add_method(builder, "implements",
        (XrCFunctionPtr)xr_type_implements, 1, 0);

    // Getters (as methods with get: prefix)
    xr_class_builder_add_method(builder, "get:name",
        (XrCFunctionPtr)xr_type_getName, 0, 0);
    xr_class_builder_add_method(builder, "get:kind",
        (XrCFunctionPtr)xr_type_getKind, 0, 0);
    xr_class_builder_add_method(builder, "get:isAbstract",
        (XrCFunctionPtr)xr_type_getIsAbstract, 0, 0);
    xr_class_builder_add_method(builder, "get:isFinal",
        (XrCFunctionPtr)xr_type_getIsFinal, 0, 0);
    xr_class_builder_add_method(builder, "get:superType",
        (XrCFunctionPtr)xr_type_getSuperType, 0, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Field Class ========== */

static XrClass* create_field_class(XrayIsolate *X) {
    XrClassBuilder *builder = xr_class_builder_new(X, "Field", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder) return NULL;

    xr_class_builder_add_method(builder, "get",
        (XrCFunctionPtr)xr_field_get, 1, 0);
    xr_class_builder_add_method(builder, "set",
        (XrCFunctionPtr)xr_field_set, 2, 0);
    xr_class_builder_add_method(builder, "getStatic",
        (XrCFunctionPtr)xr_field_getStatic, 0, 0);
    xr_class_builder_add_method(builder, "setStatic",
        (XrCFunctionPtr)xr_field_setStatic, 1, 0);

    xr_class_builder_add_method(builder, "get:name",
        (XrCFunctionPtr)xr_field_getName, 0, 0);
    xr_class_builder_add_method(builder, "get:type",
        (XrCFunctionPtr)xr_field_getType, 0, 0);
    xr_class_builder_add_method(builder, "get:isStatic",
        (XrCFunctionPtr)xr_field_getIsStatic, 0, 0);
    xr_class_builder_add_method(builder, "get:isPrivate",
        (XrCFunctionPtr)xr_field_getIsPrivate, 0, 0);
    xr_class_builder_add_method(builder, "get:isReadonly",
        (XrCFunctionPtr)xr_field_getIsReadonly, 0, 0);
    xr_class_builder_add_method(builder, "get:declaringType",
        (XrCFunctionPtr)xr_field_getDeclaringType, 0, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Method Class ========== */

static XrClass* create_method_class(XrayIsolate *X) {
    XrClassBuilder *builder = xr_class_builder_new(X, "Method", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder) return NULL;

    xr_class_builder_add_method(builder, "invoke",
        (XrCFunctionPtr)xr_method_invoke, 2, 0);
    xr_class_builder_add_method(builder, "invokeStatic",
        (XrCFunctionPtr)xr_method_invokeStatic, 1, 0);

    xr_class_builder_add_method(builder, "get:name",
        (XrCFunctionPtr)xr_method_getName, 0, 0);
    xr_class_builder_add_method(builder, "get:returnType",
        (XrCFunctionPtr)xr_method_getReturnType, 0, 0);
    xr_class_builder_add_method(builder, "get:isStatic",
        (XrCFunctionPtr)xr_method_getIsStatic, 0, 0);
    xr_class_builder_add_method(builder, "get:isPrivate",
        (XrCFunctionPtr)xr_method_getIsPrivate, 0, 0);
    xr_class_builder_add_method(builder, "get:isAbstract",
        (XrCFunctionPtr)xr_method_getIsAbstract, 0, 0);
    xr_class_builder_add_method(builder, "get:isOverride",
        (XrCFunctionPtr)xr_method_getIsOverride, 0, 0);
    xr_class_builder_add_method(builder, "get:isGetter",
        (XrCFunctionPtr)xr_method_getIsGetter, 0, 0);
    xr_class_builder_add_method(builder, "get:isSetter",
        (XrCFunctionPtr)xr_method_getIsSetter, 0, 0);
    xr_class_builder_add_method(builder, "get:isOperator",
        (XrCFunctionPtr)xr_method_getIsOperator, 0, 0);
    xr_class_builder_add_method(builder, "get:declaringType",
        (XrCFunctionPtr)xr_method_getDeclaringType, 0, 0);
    xr_class_builder_add_method(builder, "get:parameterCount",
        (XrCFunctionPtr)xr_method_getParameterCount, 0, 0);
    xr_class_builder_add_method(builder, "get:parameters",
        (XrCFunctionPtr)xr_method_getParameters, 0, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Constructor Class ========== */

static XrClass* create_constructor_class(XrayIsolate *X) {
    XrClassBuilder *builder = xr_class_builder_new(X, "Constructor", xr_isolate_get_core_classes(X)->objectClass);
    if (!builder) return NULL;

    xr_class_builder_add_method(builder, "newInstance",
        (XrCFunctionPtr)xr_constructor_newInstance, 1, 0);

    return xr_class_builder_finalize(builder);
}

/* ========== Main Initialization ========== */

void xr_reflect_api_init(XrayIsolate *X) {
    XR_DCHECK(X != NULL, "xr_reflect_api_init: NULL isolate");
    // Each create_*_class call funnels through xr_class_builder_finalize,
    // which registers the resulting class with the reflection type
    // registry as part of finalisation. The old explicit registration
    // block that followed was pure duplication and has been removed.
    XrClass *reflectClass = create_reflect_class(X);
    XrClass *typeClass = create_type_class(X);
    XrClass *fieldClass = create_field_class(X);
    XrClass *methodClass = create_method_class(X);
    XrClass *constructorClass = create_constructor_class(X);

    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    if (core) {
        core->reflectClass = reflectClass;
        core->typeClass = typeClass;
        core->fieldClass = fieldClass;
        core->methodClass = methodClass;
        core->constructorClass = constructorClass;
    }
}

