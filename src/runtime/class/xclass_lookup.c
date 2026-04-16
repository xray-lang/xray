/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_lookup.c - Fast class lookup (independent of reflection)
 *
 * KEY CONCEPT:
 *   O(1) for core classes via strcmp chain.
 *   O(1) for registered user classes via type_registry hashmap.
 *   O(n) fallback for unregistered classes via globals table scan.
 */

#include "xclass_lookup.h"
#include "../../base/xchecks.h"
#include "../xisolate_api.h"
#include "xclass.h"
#include "xreflect_registry.h"
#include "../value/xvalue.h"
#include "../xglobals_table.h"
#include "xclass_system.h"
#include "../value/xtype_names.h"
#include <string.h>

XrClass* xr_class_lookup_by_name(XrayIsolate *X, const char *class_name) {
    if (!X || !class_name) return NULL;
    
    // Fast path: core classes (O(1) strcmp chain)
    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    if (core) {
        if (strcmp(class_name, CLASS_NAME_OBJECT) == 0) return core->objectClass;
        if (strcmp(class_name, TYPE_NAME_STRING) == 0) return core->stringClass;
        if (strcmp(class_name, TYPE_NAME_ARRAY) == 0) return core->arrayClass;
        if (strcmp(class_name, TYPE_NAME_MAP) == 0) return core->mapClass;
        if (strcmp(class_name, TYPE_NAME_SET) == 0) return core->setClass;
        if (strcmp(class_name, TYPE_NAME_INT) == 0) return core->intClass;
        if (strcmp(class_name, TYPE_NAME_FLOAT) == 0) return core->floatClass;
        if (strcmp(class_name, TYPE_NAME_BOOL) == 0) return core->boolClass;
        if (strcmp(class_name, TYPE_NAME_NULL) == 0) return core->nullClass;
        if (strcmp(class_name, TYPE_NAME_BIGINT) == 0) return core->bigintClass;
        if (strcmp(class_name, CLASS_NAME_ENUM) == 0) return core->enumClass;
    }
    
    // O(1) path: lookup via type registry hashmap (covers all registered classes)
    XrTypeRegistry *registry = xr_isolate_get_type_registry(X);
    if (registry) {
        XrTypeMetadata *meta = xr_registry_find_type(X, class_name);
        if (meta && meta->klass) return meta->klass;
    }
    
    // Fallback: linear scan globals table (unregistered user classes)
    XrGlobalsTable *globals = xr_isolate_get_globals(X);
    if (!globals) return NULL;
    
    size_t count = xr_globals_count(globals);
    for (size_t i = 0; i < count; i++) {
        XrValue val = xr_globals_get(globals, (int)i);
        if (XR_IS_CLASS(val)) {
            XrClass *klass = XR_TO_CLASS(val);
            if (klass && klass->name && strcmp(klass->name, class_name) == 0) {
                return klass;
            }
        }
    }
    
    return NULL;
}
