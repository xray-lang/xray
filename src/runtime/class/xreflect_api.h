/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xreflect_api.h - Reflection API layer for xray language
 *
 * KEY CONCEPT:
 *   Provides Reflect, Type, Field, Method, Constructor classes.
 *   Methods implemented in C and bound to classes.
 */

#ifndef XREFLECT_API_H
#define XREFLECT_API_H

#include "../value/xvalue.h"

typedef struct XrTypeMetadata XrTypeMetadata;
typedef struct XrFieldMetadata XrFieldMetadata;
typedef struct XrMethodMetadata XrMethodMetadata;
typedef struct XrParameterMetadata XrParameterMetadata;

/* ========== Initialization ========== */

XR_FUNC void xr_reflect_api_init(XrayIsolate *X);
XR_FUNC void xr_reflect_api_free(XrayIsolate *X);

/* ========== Reflect Class ========== */

// Reflect.getType(obj: any): Type
XR_FUNC XrValue xr_reflect_getType(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.getTypeByName(name: string): Type?
XR_FUNC XrValue xr_reflect_getTypeByName(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.getAllTypes(): Array<Type>
XR_FUNC XrValue xr_reflect_getAllTypes(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.isInstance(obj: any, type: Type): bool
XR_FUNC XrValue xr_reflect_isInstance(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.isInstanceOf(obj: any, typeName: string): bool
XR_FUNC XrValue xr_reflect_isInstanceOf(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.fieldCount(obj: Json): int
XR_FUNC XrValue xr_reflect_fieldCount(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.elementType(obj: Array|Set|Channel): string
XR_FUNC XrValue xr_reflect_elementType(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.keyType(obj: Map): string
XR_FUNC XrValue xr_reflect_keyType(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.valueType(obj: Map): string
XR_FUNC XrValue xr_reflect_valueType(XrayIsolate *isolate, XrValue *args, int nargs);

// Reflect.typeOf(obj: any): string  (full generic type string, e.g. "Array<int>")
XR_FUNC XrValue xr_reflect_typeOf(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Type Class ========== */

XR_FUNC XrValue xr_type_getFields(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getDeclaredFields(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getField(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getMethods(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getDeclaredMethods(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getMethod(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getConstructor(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_newInstance(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_newInstanceWith(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_isSubtypeOf(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_isAssignableFrom(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_implements(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Field Class ========== */

XR_FUNC XrValue xr_field_get(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_set(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getStatic(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_setStatic(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Method Class ========== */

XR_FUNC XrValue xr_method_invoke(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_invokeStatic(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Type Property Getters ========== */

XR_FUNC XrValue xr_type_getName(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getKind(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getIsAbstract(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getIsFinal(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getSuperType(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Field Property Getters ========== */

XR_FUNC XrValue xr_field_getName(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getType(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getIsStatic(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getIsPrivate(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getIsReadonly(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getDeclaringType(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Method Property Getters ========== */

XR_FUNC XrValue xr_method_getName(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getReturnType(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsStatic(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsPrivate(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsAbstract(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsGetter(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsSetter(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsOperator(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getDeclaringType(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsOverride(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getParameterCount(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getParameters(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Parameter Property Getters ========== */

XR_FUNC XrValue xr_parameter_getName(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_parameter_getType(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_parameter_getIndex(XrayIsolate *isolate, XrValue *args, int nargs);
XR_FUNC XrValue xr_parameter_getHasDefault(XrayIsolate *isolate, XrValue *args, int nargs);

/* ========== Constructor Class ========== */

XR_FUNC XrValue xr_constructor_newInstance(XrayIsolate *isolate, XrValue *args, int nargs);

#endif  // XREFLECT_API_H
