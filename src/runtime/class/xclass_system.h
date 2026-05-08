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

    // Json utility (static methods only)
    XrClass *jsonClass;

    // Utility
    XrClass *stringBuilderClass;
    XrClass *arraySliceClass;
    XrClass *processClass;
} XrayCoreClasses;

/* ========== Lifecycle ========== */

XR_FUNC void xr_core_init(XrayIsolate *X);
XR_FUNC void xr_core_free(XrayIsolate *X);

#endif  // XCLASS_SYSTEM_H
