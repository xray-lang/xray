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

XR_FUNC void xr_reflect_api_init(XrVMRuntime *X);
XR_FUNC void xr_reflect_api_free(XrVMRuntime *X);

/* ========== Reflect Class ========== */

// Reflect.getType(obj: Json): Type
XR_FUNC XrValue xr_reflect_getType(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

// Reflect.getTypeByName(name: string): Type?
XR_FUNC XrValue xr_reflect_getTypeByName(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                         int nargs);

// Reflect.getAllTypes(): Array<Type>
XR_FUNC XrValue xr_reflect_getAllTypes(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int nargs);

// Reflect.isInstance(obj: Json, type: Type): bool
XR_FUNC XrValue xr_reflect_isInstance(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

// Reflect.isInstanceOf(obj: Json, typeName: string): bool
XR_FUNC XrValue xr_reflect_isInstanceOf(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                        int nargs);

// Reflect.fieldCount(obj: Json): int
XR_FUNC XrValue xr_reflect_fieldCount(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

// Reflect.elementType(obj: Array|Set|Channel): string
XR_FUNC XrValue xr_reflect_elementType(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int nargs);

// Reflect.keyType(obj: Map): string
XR_FUNC XrValue xr_reflect_keyType(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

// Reflect.valueType(obj: Map): string
XR_FUNC XrValue xr_reflect_valueType(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

// Reflect.typeOf(obj: Json): string  (full generic type string, e.g. "Array<int>")
XR_FUNC XrValue xr_reflect_typeOf(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

/* ========== Type Class ========== */

XR_FUNC XrValue xr_type_getFields(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getDeclaredFields(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                          int nargs);
XR_FUNC XrValue xr_type_getField(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getMethods(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getDeclaredMethods(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                           int nargs);
XR_FUNC XrValue xr_type_getMethod(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getConstructor(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int nargs);
XR_FUNC XrValue xr_type_newInstance(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_newInstanceWith(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                        int nargs);
XR_FUNC XrValue xr_type_isSubtypeOf(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_isAssignableFrom(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                         int nargs);
XR_FUNC XrValue xr_type_implements(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

/* ========== Field Class ========== */

XR_FUNC XrValue xr_field_get(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_set(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getStatic(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_setStatic(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

/* ========== Method Class ========== */

XR_FUNC XrValue xr_method_invoke(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_invokeStatic(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int nargs);

/* ========== Type Property Getters ========== */

XR_FUNC XrValue xr_type_getName(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getKind(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getIsAbstract(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getIsFinal(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_type_getSuperType(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);

/* ========== Field Property Getters ========== */

XR_FUNC XrValue xr_field_getName(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getType(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getIsStatic(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getIsPrivate(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_field_getIsReadonly(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int nargs);
XR_FUNC XrValue xr_field_getDeclaringType(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                          int nargs);

/* ========== Method Property Getters ========== */

XR_FUNC XrValue xr_method_getName(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getReturnType(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                        int nargs);
XR_FUNC XrValue xr_method_getIsStatic(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsPrivate(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                       int nargs);
XR_FUNC XrValue xr_method_getIsAbstract(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                        int nargs);
XR_FUNC XrValue xr_method_getIsGetter(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsSetter(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_method_getIsOperator(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                        int nargs);
XR_FUNC XrValue xr_method_getDeclaringType(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                           int nargs);
XR_FUNC XrValue xr_method_getIsOverride(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                        int nargs);
XR_FUNC XrValue xr_method_getParameterCount(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                            int nargs);
XR_FUNC XrValue xr_method_getParameters(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                        int nargs);

/* ========== Parameter Property Getters ========== */

XR_FUNC XrValue xr_parameter_getName(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_parameter_getType(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_parameter_getIndex(XrVMRuntime *isolate, XrValue self, XrValue *args, int nargs);
XR_FUNC XrValue xr_parameter_getHasDefault(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                           int nargs);

/* ========== Constructor Class ========== */

XR_FUNC XrValue xr_constructor_newInstance(XrVMRuntime *isolate, XrValue self, XrValue *args,
                                           int nargs);

#endif  // XREFLECT_API_H
