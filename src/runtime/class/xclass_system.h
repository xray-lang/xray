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
#include "../../shared/xr_builtin_schema.h"

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
    XrClass *enumValueClass;  // Internal class for XrEnumValue instances
    XrClass *enumTypeClass;   // Internal class for XrEnumType instances

    // Json utility (static methods only: Json.parse, Json.stringify, etc.)
    XrClass *jsonClass;

    // Json instance methods (iterator, toString, keys, values, has, etc.).
    // Wired as jsonRootClass->super so dynamic-layout instances find these
    // methods via normal class-chain lookup.
    XrClass *jsonInstanceMethodClass;

    // Dynamic-layout root classes for Json objects and Records.
    // Json inherits Json instance methods; Record does not. Sealed and open
    // Records intentionally use separate roots so hidden-class leaf flags do
    // not leak between `{x,y}` and `{x,y,...}` shapes.
    XrClass *jsonRootClass;
    XrClass *recordRootClass;
    XrClass *recordSealedRootClass;

    // Exception (populated when stdlib/types/exception.xr is loaded)
    XrClass *panicInfoClass;

    // Native-body migrated types
    XrClass *rangeClass;
    XrClass *dateTimeClass;
    XrClass *loggerClass;
    XrClass *iteratorClass;
    XrClass *regexClass;
    XrClass *regexMatchClass;
    XrClass *sysMutexClass;
    XrClass *sysRwLockClass;
    XrClass *sysCondvarClass;
    XrClass *sysBarrierClass;
    XrClass *sysOnceClass;
    XrClass *netConnClass;
    XrClass *netListenerClass;

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
XR_FUNC struct XrClass *xr_get_or_create_tuple_class(XrVMRuntime *X, uint16_t arity);

/* Exception field indices — must match stdlib/types/exception.xr layout */
#define PANIC_INFO_FIELD_MESSAGE 0
#define PANIC_INFO_FIELD_STACK 1
#define PANIC_INFO_FIELD_CAUSE 2
#define PANIC_INFO_FIELD_CODE 3
#define PANIC_INFO_FIELD_DATA 4

/* ========== Lifecycle ========== */

XR_FUNC void xr_core_init(XrVMRuntime *X);
XR_FUNC void xr_core_free(XrVMRuntime *X);

#endif  // XCLASS_SYSTEM_H
