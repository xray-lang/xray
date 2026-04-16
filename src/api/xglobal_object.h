/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_object.h - Global object management
 *
 * KEY CONCEPT:
 *   Global object holds core classes and builtin functions accessible
 *   from any scope. Each Isolate has its own global object.
 *
 * RELATED MODULES:
 *   - xray_isolate.h: Owns the global object
 *   - xclass_system.h: Core classes registered here
 */

#ifndef XGLOBAL_OBJECT_H
#define XGLOBAL_OBJECT_H

#include <stdbool.h>
#include "../base/xhashmap.h"

#include "../base/xforward_decl.h"

/* ========== Global Object Structure ========== */

typedef struct XrGlobalObject {
    XrayIsolate* isolate;
    XrHashMap* properties;              // Core classes (String, Array, etc.)
    XrHashMap* functions;               // Builtin functions (reserved)
    int registered_class_count;
} XrGlobalObject;

/* ========== Global Object API ========== */

XR_FUNC XrGlobalObject* xr_global_object_create(XrayIsolate* isolate);
XR_FUNC void xr_global_object_destroy(XrGlobalObject* global);

XR_FUNC bool xr_global_register_class(XrGlobalObject* global, 
                               const char* name, 
                               XrClass* klass);

// Register all core classes (Object, String, Array, etc.)
XR_FUNC bool xr_global_register_all_core_classes(XrGlobalObject* global,
                                          XrayIsolate* isolate);

XR_FUNC bool xr_global_register_all_builtin_functions(XrGlobalObject* global);

#endif // XGLOBAL_OBJECT_H
