/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_type.h - Native type registration system
 *
 * KEY CONCEPT:
 *   Registers C native types (like DateTime, Regex) as XrClass.
 *   Provides consistent method call experience with xray classes.
 *
 * WHY THIS DESIGN:
 *   High-frequency built-in types use VM fast path.
 *   This system is for stdlib extensions and third-party native extensions.
 */

#ifndef XNATIVE_TYPE_H
#define XNATIVE_TYPE_H

#include "../class/xclass.h"
#include "../class/xmethod.h"
#include "../gc/xgc_header.h"

/* ========== Native Method Definition ========== */

// Native method descriptor (array ends with {NULL, NULL, 0})
typedef struct XrNativeMethod {
    const char *name;     // Method name
    XrCFunctionPtr func;  // C function pointer
    int arity;            // Parameter count (including this, -1 for variadic)
} XrNativeMethod;

/* ========== Native Type Registration Info ========== */

// Native type registration descriptor
typedef struct XrNativeTypeInfo {
    const char *name;                // Type name (e.g. "DateTime")
    XrObjType gc_type;               // GC type ID (e.g. XR_TDATETIME)
    XrNativeMethod *methods;         // Instance methods (NULL-terminated)
    XrNativeMethod *getters;         // Property getters (NULL-terminated, optional)
    XrNativeMethod *static_methods;  // Static methods (NULL-terminated, optional)
} XrNativeTypeInfo;

/* ========== Type Mapping Table Capacity ========== */

// Max native type count (should be greater than XrObjType enum max)
#define XR_NATIVE_TYPE_MAX 64

/* ========== Registration API ========== */

// Register native type, generate corresponding XrClass
XR_FUNC XrClass *xr_register_native_type(XrayIsolate *isolate, const XrNativeTypeInfo *info);

// Get XrClass for native type (NULL if not registered)
XR_FUNC XrClass *xr_get_native_type_class(XrayIsolate *isolate, XrObjType gc_type);

// Check if type is registered
XR_FUNC bool xr_is_native_type_registered(XrayIsolate *isolate, XrObjType gc_type);

#endif  // XNATIVE_TYPE_H
