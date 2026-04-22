/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xglobal_object.c - Global object management implementation
 */

#include "xglobal_object.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "../runtime/xisolate_api.h"
#include "../runtime/class/xclass_system.h"
#include "../runtime/value/xtype_names.h"
#include "../base/xhashmap.h"
#include "../runtime/value/xvalue.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../base/xmalloc.h"

/* ========== Global Object Creation and Destruction ========== */

XrGlobalObject* xr_global_object_create(XrayIsolate* isolate) {
    xray_api_checkr(isolate != NULL, "global_object_create: NULL isolate", NULL);

    XrGlobalObject* global = (XrGlobalObject*)xr_malloc(sizeof(XrGlobalObject));
    if (global == NULL) {
        xr_log_warning("global", "global_object_create: failed to allocate global object");
        return NULL;
    }

    global->isolate = isolate;
    global->registered_class_count = 0;

    global->properties = xr_hashmap_new();
    if (global->properties == NULL) {
        xr_log_warning("global", "global_object_create: failed to create properties map");
        xr_free(global);
        return NULL;
    }

    global->functions = xr_hashmap_new();
    if (global->functions == NULL) {
        xr_log_warning("global", "global_object_create: failed to create functions map");
        xr_hashmap_free(global->properties);
        xr_free(global);
        return NULL;
    }

    return global;
}

void xr_global_object_destroy(XrGlobalObject* global) {
    if (global == NULL) return;

    /* xr_hashmap_free only frees the entries array and map struct.
    ** Keys are borrowed const strings (TYPE_NAME_* macros), not owned.
    ** Values are class/function pointers owned by isolate core. */
    if (global->properties) {
        xr_hashmap_free(global->properties);
        global->properties = NULL;
    }
    if (global->functions) {
        xr_hashmap_free(global->functions);
        global->functions = NULL;
    }
    xr_free(global);
}

/* ========== Registration API ========== */

bool xr_global_register_class(XrGlobalObject* global,
                               const char* name,
                               XrClass* klass) {
    if (global == NULL || name == NULL || klass == NULL) {
        return false;
    }

    xr_hashmap_set(global->properties, name, (void*)klass);

    global->registered_class_count++;
    return true;
}

bool xr_global_register_all_core_classes(XrGlobalObject* global,
                                          XrayIsolate* isolate) {
    if (global == NULL || isolate == NULL || xr_isolate_get_core_classes(isolate) == NULL) {
        xr_log_warning("global", "register_all_core_classes: invalid parameters");
        return false;
    }

    XrayCoreClasses* core = xr_isolate_get_core_classes(isolate);

    xr_global_register_class(global, CLASS_NAME_OBJECT, core->objectClass);

    xr_global_register_class(global, TYPE_NAME_STRING, core->stringClass);
    xr_global_register_class(global, TYPE_NAME_ARRAY, core->arrayClass);
    xr_global_register_class(global, TYPE_NAME_MAP, core->mapClass);
    xr_global_register_class(global, TYPE_NAME_SET, core->setClass);

    xr_global_register_class(global, TYPE_NAME_INT, core->intClass);
    xr_global_register_class(global, TYPE_NAME_FLOAT, core->floatClass);
    xr_global_register_class(global, TYPE_NAME_BOOL, core->boolClass);
    xr_global_register_class(global, TYPE_NAME_NULL, core->nullClass);

    xr_global_register_class(global, TYPE_NAME_FUNCTION, core->functionClass);
    xr_global_register_class(global, TYPE_NAME_CLOSURE, core->closureClass);
    xr_global_register_class(global, TYPE_NAME_UPVALUE, core->upvalueClass);
    xr_global_register_class(global, TYPE_NAME_CFUNCTION, core->cfunctionClass);

    xr_global_register_class(global, CLASS_NAME_REFLECT, core->reflectClass);
    xr_global_register_class(global, CLASS_NAME_TYPE, core->typeClass);
    xr_global_register_class(global, CLASS_NAME_FIELD, core->fieldClass);
    xr_global_register_class(global, CLASS_NAME_METHOD, core->methodClass);
    xr_global_register_class(global, CLASS_NAME_CONSTRUCTOR, core->constructorClass);
    xr_global_register_class(global, CLASS_NAME_PARAMETER, core->parameterClass);

    xr_global_register_class(global, CLASS_NAME_ENUM, core->enumClass);

    xr_global_register_class(global, TYPE_NAME_STRINGBUILDER, core->stringBuilderClass);

    return true;
}

bool xr_global_register_all_builtin_functions(XrGlobalObject* global) {
    if (global == NULL) return false;
    // Builtin functions (print, etc.) are handled by VM opcodes directly.
    return true;
}
