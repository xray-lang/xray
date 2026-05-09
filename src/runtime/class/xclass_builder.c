/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_builder.c - Class builder implementation
 */

#include "xclass_builder.h"
#include "xclass_builder_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xclass.h"
#include "xclass_internal.h"
#include "xmethod.h"
#include "xreflect_cache.h"
#include "xreflect_registry.h"
#include "../object/xstring.h"
#include "../xerror.h"
#include "../../base/xmalloc.h"
#include "../xisolate_api.h"
#include "../symbol/xsymbol_table.h"
#include "../gc/xgc_header.h"
#include "../gc/xsystem_heap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XrClosure XrClosure;

/* ========== Helpers ========== */

// Resolve a name through the symbol table once, returning both the
// SymbolId and the interned (stable) name pointer. All field/method
// name storage in the class module goes through this intern pool; the
// char* returned here must never be freed by callers.
static inline void xr_builder_intern_name(XrClassBuilder *builder, const char *name,
                                          int *out_symbol, const char **out_name) {
    XrSymbolTable *table = (XrSymbolTable *) xr_isolate_get_symbol_table(builder->isolate);
    SymbolId id = xr_symbol_register_in_table(table, name);
    *out_symbol = (id == SYMBOL_INVALID) ? 0 : (int) id;
    *out_name = (id == SYMBOL_INVALID)
                    ? name  // fall back to caller-owned literal; rare failure path
                    : xr_symbol_get_name_in_table(table, id);
}

// Return a stable interned pointer for a class name (no symbol id needed).
static inline const char *xr_builder_intern_class_name(XrClassBuilder *builder, const char *name) {
    const char *interned = xr_symbol_intern(builder->isolate, name);
    return interned ? interned : name;
}

/* ========== Initial Capacities ========== */

#define INITIAL_FIELD_CAPACITY 8
#define INITIAL_METHOD_CAPACITY 16
#define INITIAL_STATIC_FIELD_CAPACITY 4
#define INITIAL_STATIC_METHOD_CAPACITY 8
#define INITIAL_INTERFACE_CAPACITY 4
#define INITIAL_ABSTRACT_METHOD_CAPACITY 4

#define ENSURE_NOT_FINALIZED(builder)                                                              \
    if ((builder)->finalized) {                                                                    \
        xr_log_warning("class", "ClassBuilder: already finalized");                                \
        return -1;                                                                                 \
    }

/* ========== Dynamic Array Resize ========== */

static void *resize_array(void *array, int *capacity, size_t elem_size) {
    int new_capacity = (*capacity) * 2;
    void *new_array = xr_realloc(array, new_capacity * elem_size);
    if (new_array == NULL) {
        xr_log_warning("class", "failed to resize array");
        return NULL;
    }
    *capacity = new_capacity;
    return new_array;
}

/* ========== Builder Creation ========== */

XrClassBuilder *xr_class_builder_new(XrayIsolate *isolate, const char *name, XrClass *super) {
    XR_DCHECK(isolate != NULL, "class_builder_new: NULL isolate");
    XR_DCHECK(name != NULL, "class_builder_new: NULL name");
    if (isolate == NULL || name == NULL) {
        xr_log_warning("class", "class_builder_new: invalid arguments");
        return NULL;
    }

    XrClassBuilder *builder = (XrClassBuilder *) xr_malloc(sizeof(XrClassBuilder));
    if (builder == NULL) {
        xr_log_warning("class", "class_builder_new: failed to allocate builder");
        return NULL;
    }

    memset(builder, 0, sizeof(XrClassBuilder));

    builder->isolate = isolate;
    // Class name is interned; ownership belongs to the symbol table, not
    // the builder. Finalize copies this pointer into cls->name as-is.
    builder->name = xr_builder_intern_class_name(builder, name);
    builder->super = super;
    builder->finalized = false;

    builder->field_capacity = INITIAL_FIELD_CAPACITY;
    builder->fields =
        (XrFieldBuildItem *) xr_calloc(builder->field_capacity, sizeof(XrFieldBuildItem));

    builder->method_capacity = INITIAL_METHOD_CAPACITY;
    builder->methods =
        (XrMethodBuildItem *) xr_calloc(builder->method_capacity, sizeof(XrMethodBuildItem));

    builder->static_field_capacity = INITIAL_STATIC_FIELD_CAPACITY;
    builder->static_fields = (XrStaticFieldBuildItem *) xr_calloc(builder->static_field_capacity,
                                                                  sizeof(XrStaticFieldBuildItem));

    builder->static_method_capacity = INITIAL_STATIC_METHOD_CAPACITY;
    builder->static_methods =
        (XrMethodBuildItem *) xr_calloc(builder->static_method_capacity, sizeof(XrMethodBuildItem));

    builder->interface_capacity = INITIAL_INTERFACE_CAPACITY;
    builder->interfaces = (XrClass **) xr_calloc(builder->interface_capacity, sizeof(XrClass *));

    builder->abstract_method_capacity = INITIAL_ABSTRACT_METHOD_CAPACITY;
    builder->abstract_methods = (int *) xr_calloc(builder->abstract_method_capacity, sizeof(int));

    if (!builder->fields || !builder->methods || !builder->static_fields || !builder->interfaces ||
        !builder->abstract_methods) {
        xr_class_builder_destroy(builder);
        return NULL;
    }

    return builder;
}

/* ========== Field Operations ========== */

bool xr_class_builder_has_field(const XrClassBuilder *builder, const char *name) {
    XR_DCHECK(builder != NULL, "has_field: NULL builder");
    // Every add_field call intern's its name via the isolate's symbol
    // table, so the builder's fields[] names are all interned
    // pointers. Interning the caller's name once gives us the same
    // canonical pointer and reduces duplicate-check to a single
    // pointer comparison per slot.
    const char *interned = xr_symbol_intern(builder->isolate, name);
    if (!interned)
        return false;
    for (int i = 0; i < builder->field_count; i++) {
        if (builder->fields[i].name == interned) {
            return true;
        }
    }
    return false;
}

int xr_class_builder_add_field(XrClassBuilder *builder, const char *name, uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_field: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_field: invalid arguments");
        return -1;
    }

    if (xr_class_builder_has_field(builder, name)) {
        xr_log_warning("class", "add_field: duplicate field '%s'", name);
        return -1;
    }

    if (builder->field_count >= builder->field_capacity) {
        builder->fields = (XrFieldBuildItem *) resize_array(
            builder->fields, &builder->field_capacity, sizeof(XrFieldBuildItem));
        if (builder->fields == NULL)
            return -1;
    }

    XrFieldBuildItem *item = &builder->fields[builder->field_count];
    xr_builder_intern_name(builder, name, &item->symbol, &item->name);
    item->default_value = xr_null();
    item->flags = flags;
    item->offset = 0;

    builder->field_count++;
    XR_DCHECK(builder->field_count <= builder->field_capacity, "add_field: count > capacity");
    return 0;
}

/* ========== Method Operations ========== */

bool xr_class_builder_has_method(const XrClassBuilder *builder, const char *name) {
    XR_DCHECK(builder != NULL, "has_method: NULL builder");
    const char *interned = xr_symbol_intern(builder->isolate, name);
    if (!interned)
        return false;
    for (int i = 0; i < builder->method_count; i++) {
        if (builder->methods[i].name == interned) {
            return true;
        }
    }
    return false;
}

int xr_class_builder_add_method(XrClassBuilder *builder, const char *name, XrPrimitiveMethodFn impl,
                                int param_count, uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_method: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_method: invalid arguments");
        return -1;
    }

    if (xr_class_builder_has_method(builder, name)) {
        xr_log_warning("class", "add_method: duplicate method '%s'", name);
        return -1;
    }

    if (builder->method_count >= builder->method_capacity) {
        builder->methods = (XrMethodBuildItem *) resize_array(
            builder->methods, &builder->method_capacity, sizeof(XrMethodBuildItem));
        if (builder->methods == NULL)
            return -1;
    }

    XrMethodBuildItem *item = &builder->methods[builder->method_count];
    xr_builder_intern_name(builder, name, &item->symbol, &item->name);
    item->method_type = XMETHOD_PRIMITIVE;
    item->impl.primitive = impl;
    item->param_count = param_count;
    item->flags = flags;
    item->op_type = 0;

    builder->method_count++;
    XR_DCHECK(builder->method_count <= builder->method_capacity, "add_method: count > capacity");
    return 0;
}

int xr_class_builder_add_method_closure(XrClassBuilder *builder, const char *name,
                                        XrClosure *closure, XrMethodType method_type,
                                        int param_count, uint32_t flags, uint8_t op_type) {
    XR_DCHECK(builder != NULL, "add_method_closure: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_method_closure: invalid arguments");
        return -1;
    }

    if (builder->method_count >= builder->method_capacity) {
        builder->methods = (XrMethodBuildItem *) resize_array(
            builder->methods, &builder->method_capacity, sizeof(XrMethodBuildItem));
        if (builder->methods == NULL)
            return -1;
    }

    XrMethodBuildItem *item = &builder->methods[builder->method_count];
    xr_builder_intern_name(builder, name, &item->symbol, &item->name);
    item->method_type = method_type;
    item->impl.closure = closure;
    item->param_count = param_count;
    item->flags = flags;
    item->op_type = op_type;

    builder->method_count++;
    return 0;
}

/* ========== Static Field Operations ========== */

int xr_class_builder_add_static_field(XrClassBuilder *builder, const char *name, XrValue value,
                                      uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_static_field: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_static_field: invalid arguments");
        return -1;
    }

    if (builder->static_field_count >= builder->static_field_capacity) {
        builder->static_fields = (XrStaticFieldBuildItem *) resize_array(
            builder->static_fields, &builder->static_field_capacity,
            sizeof(XrStaticFieldBuildItem));
        if (builder->static_fields == NULL)
            return -1;
    }

    XrStaticFieldBuildItem *item = &builder->static_fields[builder->static_field_count];
    xr_builder_intern_name(builder, name, &item->symbol, &item->name);
    item->value = value;
    item->flags = flags;

    builder->static_field_count++;
    return 0;
}

/* ========== Static Method Operations ========== */

int xr_class_builder_add_static_method(XrClassBuilder *builder, const char *name,
                                       XrPrimitiveMethodFn impl, int param_count, uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_static_method: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_static_method: invalid arguments");
        return -1;
    }

    if (builder->static_method_count >= builder->static_method_capacity) {
        builder->static_methods = (XrMethodBuildItem *) resize_array(
            builder->static_methods, &builder->static_method_capacity, sizeof(XrMethodBuildItem));
        if (builder->static_methods == NULL)
            return -1;
    }

    XrMethodBuildItem *item = &builder->static_methods[builder->static_method_count];
    xr_builder_intern_name(builder, name, &item->symbol, &item->name);
    item->method_type = XMETHOD_PRIMITIVE;
    item->impl.primitive = impl;
    item->param_count = param_count;
    item->op_type = 0;
    item->flags = flags | XMETHOD_FLAG_STATIC;

    builder->static_method_count++;
    return 0;
}

int xr_class_builder_add_static_method_closure(XrClassBuilder *builder, const char *name,
                                               XrClosure *closure, int param_count,
                                               uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_static_method_closure: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_static_method_closure: invalid arguments");
        return -1;
    }

    if (builder->static_method_count >= builder->static_method_capacity) {
        builder->static_methods = (XrMethodBuildItem *) resize_array(
            builder->static_methods, &builder->static_method_capacity, sizeof(XrMethodBuildItem));
        if (builder->static_methods == NULL)
            return -1;
    }

    XrMethodBuildItem *item = &builder->static_methods[builder->static_method_count];
    xr_builder_intern_name(builder, name, &item->symbol, &item->name);
    item->method_type = XMETHOD_CLOSURE;
    item->impl.closure = closure;
    item->param_count = param_count;
    item->op_type = 0;
    item->flags = flags | XMETHOD_FLAG_STATIC;

    builder->static_method_count++;
    return 0;
}

/* ========== Interface Operations ========== */

int xr_class_builder_add_interface(XrClassBuilder *builder, XrClass *interface) {
    XR_DCHECK(builder != NULL, "add_interface: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (interface == NULL) {
        xr_log_warning("class", "add_interface: invalid interface");
        return -1;
    }

    if (builder->interface_count >= builder->interface_capacity) {
        builder->interfaces = (XrClass **) resize_array(
            builder->interfaces, &builder->interface_capacity, sizeof(XrClass *));
        if (builder->interfaces == NULL)
            return -1;
    }

    builder->interfaces[builder->interface_count] = interface;
    builder->interface_count++;

    return 0;
}

/* ========== Abstract Method Operations ========== */

int xr_class_builder_add_abstract_method(XrClassBuilder *builder, int method_symbol) {
    XR_DCHECK(builder != NULL, "add_abstract_method: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (builder->abstract_method_count >= builder->abstract_method_capacity) {
        builder->abstract_methods = (int *) resize_array(
            builder->abstract_methods, &builder->abstract_method_capacity, sizeof(int));
        if (builder->abstract_methods == NULL)
            return -1;
    }

    builder->abstract_methods[builder->abstract_method_count] = method_symbol;
    builder->abstract_method_count++;
    builder->flags |= XR_CLASS_ABSTRACT;

    return 0;
}

/* ========== Flags ========== */

void xr_class_builder_set_flags(XrClassBuilder *builder, uint32_t flags) {
    if (builder != NULL) {
        builder->flags |= flags;
    }
}

/* ========== Monomorphized Generics ========== */

void xr_class_builder_set_display_name(XrClassBuilder *builder, const char *display_name) {
    XR_DCHECK(builder != NULL, "set_display_name: NULL builder");
    if (builder != NULL) {
        builder->display_name = display_name;
    }
}

void xr_class_builder_set_generic_origin(XrClassBuilder *builder, XrClass *origin) {
    XR_DCHECK(builder != NULL, "set_generic_origin: NULL builder");
    if (builder != NULL) {
        builder->generic_origin = origin;
    }
}

void xr_class_builder_set_mono_type_arg_names(XrClassBuilder *builder, const char **type_arg_names,
                                              uint8_t argc) {
    XR_DCHECK(builder != NULL, "set_mono_type_arg_names: NULL builder");
    if (builder == NULL || argc == 0 || type_arg_names == NULL)
        return;
    const char **copy = (const char **) xr_malloc(argc * sizeof(const char *));
    if (copy == NULL)
        return;
    for (uint8_t i = 0; i < argc; i++)
        copy[i] = type_arg_names[i] ? xr_strdup(type_arg_names[i]) : NULL;
    /* Free previous names if any */
    if (builder->mono_type_arg_names) {
        for (uint8_t i = 0; i < builder->mono_type_argc; i++)
            xr_free((void *) builder->mono_type_arg_names[i]);
        xr_free(builder->mono_type_arg_names);
    }
    builder->mono_type_arg_names = copy;
    builder->mono_type_argc = argc;
}

// calculate_instance_size was a dead public API -- the actual instance
// size computation lives inside finalize_fields in
// xclass_builder_finalize.c, and no external consumer ever asked for a
// pre-finalize preview.
//
// generate_vtable + find_method_in_parent_vtable moved to
// xclass_builder_finalize.c (file-local static there) because the
// only caller is finalize_methods; there are no cross-TU users.

/* ========== Builder Destroy ========== */

void xr_class_builder_destroy(XrClassBuilder *builder) {
    if (builder == NULL)
        return;

    // All name fields below point into the symbol table's intern pool and
    // must not be freed here; only the dynamic arrays themselves are ours.
    xr_free(builder->fields);
    xr_free(builder->methods);
    xr_free(builder->static_fields);
    xr_free(builder->static_methods);
    xr_free(builder->interfaces);
    xr_free(builder->abstract_methods);
    if (builder->mono_type_arg_names) {
        for (uint8_t i = 0; i < builder->mono_type_argc; i++)
            xr_free((void *) builder->mono_type_arg_names[i]);
        xr_free(builder->mono_type_arg_names);
    }
    xr_free(builder);
}
