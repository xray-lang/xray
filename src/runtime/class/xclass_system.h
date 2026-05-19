/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_system.h - Global class hierarchy
 *
 * KEY CONCEPT:
 *   Core class hierarchy: Object, String, Array, Map, etc.
 *   Bootstrap mechanism solves Class-Object circular dependency.
 *   All objects have classObj reference.
 */

#ifndef XCLASS_SYSTEM_H
#define XCLASS_SYSTEM_H

#include "../value/xvalue.h"

// Forward declarations via xforward_decl.h

typedef struct XrayCoreClasses {
    // Base
    XrClass *objectClass;

    // Builtin types
    XrClass *stringClass;
    XrClass *arrayClass;
    XrClass *mapClass;
    XrClass *setClass;
    XrClass *intClass;
    XrClass *floatClass;
    XrClass *boolClass;
    XrClass *nullClass;
    XrClass *bigintClass;

    // Function related
    XrClass *functionClass;
    XrClass *closureClass;
    XrClass *upvalueClass;
    XrClass *cfunctionClass;

    // Reflection API
    XrClass *reflectClass;
    XrClass *typeClass;
    XrClass *fieldClass;
    XrClass *methodClass;
    XrClass *constructorClass;
    XrClass *parameterClass;

    // Enum
    XrClass *enumClass;

    // Json utility (static methods only: Json.parse, Json.stringify, etc.)
    XrClass *jsonClass;

    // Json instance methods (iterator, toString, keys, values, has, etc.).
    // Wired as jsonRootClass->super so dynamic-layout instances find these
    // methods via normal class-chain lookup.
    XrClass *jsonInstanceMethodClass;

    // Dynamic-layout root classes for Json objects.
    // jsonRootClass: open Json (transitions create child classes on field add)
    // jsonRootSealedClass: prototype for sealed Json types — each sealed
    //   compile-time Json<{...}> derives its own sealed class via transitions
    //   from this root and toggling XR_CLASS_DYNAMIC_SEALED.
    XrClass *jsonRootClass;

    // Exception (populated when stdlib/types/exception.xr is loaded)
    XrClass *exceptionClass;

    // Native-body migrated types
    XrClass *rangeClass;
    XrClass *dateTimeClass;
    XrClass *loggerClass;

    // Tuples: one XrClass per arity (lazy-built on first use). Each class
    // declares N untyped fields whose slot is tuple element i. Arities
    // above XR_TUPLE_CLASS_PREALLOC fall back to a slower lookup map.
    XrClass *tupleClassesSmall[32];

    // Utility
    XrClass *stringBuilderClass;
    XrClass *processClass;
} XrayCoreClasses;

#define XR_TUPLE_CLASS_PREALLOC 32

/* Return the cached tuple class for `arity`, creating it on demand.
 * Returns NULL on allocation failure. Arities up to
 * XR_TUPLE_CLASS_PREALLOC-1 hit the inline cache slot directly; larger
 * arities allocate a fresh class each call (cold path, never measured
 * to matter in practice). */
XR_FUNC struct XrClass *xr_get_or_create_tuple_class(XrayIsolate *X, uint16_t arity);

/* Exception field indices — must match stdlib/types/exception.xr layout */
#define EXCEPTION_FIELD_MESSAGE 0
#define EXCEPTION_FIELD_STACK 1
#define EXCEPTION_FIELD_CAUSE 2
#define EXCEPTION_FIELD_CODE 3
#define EXCEPTION_FIELD_DATA 4

/* ========== Lifecycle ========== */

XR_FUNC void xr_core_init(XrayIsolate *X);
XR_FUNC void xr_core_free(XrayIsolate *X);

#endif  // XCLASS_SYSTEM_H
