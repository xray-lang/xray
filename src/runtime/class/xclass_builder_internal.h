/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_builder_internal.h - Internal layout of XrClassBuilder
 *
 * SCOPE:
 *   Only xclass_builder.c (and any future builder-split module)
 *   should include this file. All external callers live happily
 *   with the opaque typedef exported from xclass_builder.h.
 *
 * WHY SPLIT:
 *   The build items (XrFieldBuildItem, XrMethodBuildItem, ...) and
 *   the builder struct itself are transient, mutable scaffolding.
 *   Letting VM, JIT or reflection modules reach into them would
 *   leak mutable state into code paths that only ever need to call
 *   xr_class_builder_new + add_* + finalize.
 */

#ifndef XCLASS_BUILDER_INTERNAL_H
#define XCLASS_BUILDER_INTERNAL_H

#include "xclass_builder.h"
#include "xmethod.h"
#include "../value/xvalue.h"
#include <stdbool.h>
#include <stdint.h>

/* ========== Build Item Containers ========== */

typedef struct XrFieldBuildItem {
    const char *name;   // Interned in symbol table; not owned.
    int symbol;
    XrValue default_value;
    uint32_t flags;
    uint16_t offset;         // Computed on finalize
} XrFieldBuildItem;

typedef struct XrMethodBuildItem {
    const char *name;   // Interned in symbol table; not owned.
    int symbol;
    XrMethodType method_type;    // CLOSURE/PRIMITIVE/GETTER/SETTER/OPERATOR
    union {
        XrCFunctionPtr primitive;
        XrClosure *closure;
    } impl;
    int param_count;
    uint32_t flags;
    uint8_t op_type;             // for operator methods
} XrMethodBuildItem;

typedef struct XrStaticFieldBuildItem {
    const char *name;   // Interned in symbol table; not owned.
    int symbol;
    XrValue value;
    uint32_t flags;
} XrStaticFieldBuildItem;

/* ========== Class Builder Structure ========== */

/*
 * Lifecycle:
 *   1. xr_class_builder_new()      - Create
 *   2. xr_class_builder_add_xxx()  - Add members
 *   3. xr_class_builder_finalize() - Freeze into immutable class, destroy builder
 */
struct XrClassBuilder {
    // Basic info
    XrayIsolate *isolate;
    const char *name;   // Interned in symbol table; not owned.
    XrClass *super;

    // Fields (dynamic array)
    XrFieldBuildItem *fields;
    int field_count;
    int field_capacity;

    // Methods (dynamic array)
    XrMethodBuildItem *methods;
    int method_count;
    int method_capacity;

    // Static fields (dynamic array)
    XrStaticFieldBuildItem *static_fields;
    int static_field_count;
    int static_field_capacity;

    // Static methods (dynamic array)
    XrMethodBuildItem *static_methods;
    int static_method_count;
    int static_method_capacity;

    // Interfaces (dynamic array)
    XrClass **interfaces;
    int interface_count;
    int interface_capacity;

    // Abstract methods (dynamic array)
    int *abstract_methods;
    int abstract_method_count;
    int abstract_method_capacity;

    // Flags
    uint32_t flags;
    bool finalized;
};

#endif // XCLASS_BUILDER_INTERNAL_H
