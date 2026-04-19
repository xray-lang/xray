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
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "xclass.h"
#include "xmethod.h"
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
static inline void xr_builder_intern_name(XrClassBuilder *builder,
                                          const char *name,
                                          int *out_symbol,
                                          const char **out_name) {
    XrSymbolTable *table = (XrSymbolTable*)xr_isolate_get_symbol_table(builder->isolate);
    SymbolId id = xr_symbol_register_in_table(table, name);
    *out_symbol = (id == SYMBOL_INVALID) ? 0 : (int)id;
    *out_name = (id == SYMBOL_INVALID)
        ? name  // fall back to caller-owned literal; rare failure path
        : xr_symbol_get_name_in_table(table, id);
}

// Return a stable interned pointer for a class name (no symbol id needed).
static inline const char* xr_builder_intern_class_name(XrClassBuilder *builder,
                                                       const char *name) {
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

#define ENSURE_NOT_FINALIZED(builder) \
    if ((builder)->finalized) { \
        xr_log_warning("class", "ClassBuilder: already finalized"); \
        return -1; \
    }

/* ========== Dynamic Array Resize ========== */

static void* resize_array(void *array, int *capacity, size_t elem_size) {
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

XrClassBuilder* xr_class_builder_new(XrayIsolate *isolate,
                                      const char *name,
                                      XrClass *super) {
    XR_DCHECK(isolate != NULL, "class_builder_new: NULL isolate");
    XR_DCHECK(name != NULL, "class_builder_new: NULL name");
    if (isolate == NULL || name == NULL) {
        xr_log_warning("class", "class_builder_new: invalid arguments");
        return NULL;
    }

    XrClassBuilder *builder = (XrClassBuilder*)xr_malloc(sizeof(XrClassBuilder));
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
    builder->fields = (XrFieldBuildItem*)xr_calloc(builder->field_capacity,
                                                    sizeof(XrFieldBuildItem));

    builder->method_capacity = INITIAL_METHOD_CAPACITY;
    builder->methods = (XrMethodBuildItem*)xr_calloc(builder->method_capacity,
                                                      sizeof(XrMethodBuildItem));

    builder->static_field_capacity = INITIAL_STATIC_FIELD_CAPACITY;
    builder->static_fields = (XrStaticFieldBuildItem*)xr_calloc(
        builder->static_field_capacity, sizeof(XrStaticFieldBuildItem));

    builder->static_method_capacity = INITIAL_STATIC_METHOD_CAPACITY;
    builder->static_methods = (XrMethodBuildItem*)xr_calloc(
        builder->static_method_capacity, sizeof(XrMethodBuildItem));

    builder->interface_capacity = INITIAL_INTERFACE_CAPACITY;
    builder->interfaces = (XrClass**)xr_calloc(builder->interface_capacity,
                                                sizeof(XrClass*));

    builder->abstract_method_capacity = INITIAL_ABSTRACT_METHOD_CAPACITY;
    builder->abstract_methods = (int*)xr_calloc(builder->abstract_method_capacity,
                                                 sizeof(int));

    if (!builder->fields || !builder->methods || !builder->static_fields ||
        !builder->interfaces || !builder->abstract_methods) {
        xr_class_builder_destroy(builder);
        return NULL;
    }

    return builder;
}

/* ========== Field Operations ========== */

bool xr_class_builder_has_field(const XrClassBuilder *builder, const char *name) {
    XR_DCHECK(builder != NULL, "has_field: NULL builder");
    for (int i = 0; i < builder->field_count; i++) {
        if (strcmp(builder->fields[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

int xr_class_builder_add_field(XrClassBuilder *builder,
                                const char *name,
                                uint32_t flags) {
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
        builder->fields = (XrFieldBuildItem*)resize_array(
            builder->fields, &builder->field_capacity, sizeof(XrFieldBuildItem));
        if (builder->fields == NULL) return -1;
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
    for (int i = 0; i < builder->method_count; i++) {
        if (strcmp(builder->methods[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

int xr_class_builder_add_method(XrClassBuilder *builder,
                                 const char *name,
                                 XrCFunctionPtr impl,
                                 int param_count,
                                 uint32_t flags) {
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
        builder->methods = (XrMethodBuildItem*)resize_array(
            builder->methods, &builder->method_capacity, sizeof(XrMethodBuildItem));
        if (builder->methods == NULL) return -1;
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

int xr_class_builder_add_method_closure(XrClassBuilder *builder,
                                         const char *name,
                                         XrClosure *closure,
                                         XrMethodType method_type,
                                         int param_count,
                                         uint32_t flags,
                                         uint8_t op_type) {
    XR_DCHECK(builder != NULL, "add_method_closure: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_method_closure: invalid arguments");
        return -1;
    }

    if (builder->method_count >= builder->method_capacity) {
        builder->methods = (XrMethodBuildItem*)resize_array(
            builder->methods, &builder->method_capacity, sizeof(XrMethodBuildItem));
        if (builder->methods == NULL) return -1;
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

int xr_class_builder_add_static_field(XrClassBuilder *builder,
                                       const char *name,
                                       XrValue value,
                                       uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_static_field: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_static_field: invalid arguments");
        return -1;
    }

    if (builder->static_field_count >= builder->static_field_capacity) {
        builder->static_fields = (XrStaticFieldBuildItem*)resize_array(
            builder->static_fields, &builder->static_field_capacity,
            sizeof(XrStaticFieldBuildItem));
        if (builder->static_fields == NULL) return -1;
    }

    XrStaticFieldBuildItem *item = &builder->static_fields[builder->static_field_count];
    xr_builder_intern_name(builder, name, &item->symbol, &item->name);
    item->value = value;
    item->flags = flags;

    builder->static_field_count++;
    return 0;
}

/* ========== Static Method Operations ========== */

int xr_class_builder_add_static_method(XrClassBuilder *builder,
                                        const char *name,
                                        XrCFunctionPtr impl,
                                        int param_count,
                                        uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_static_method: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_static_method: invalid arguments");
        return -1;
    }

    if (builder->static_method_count >= builder->static_method_capacity) {
        builder->static_methods = (XrMethodBuildItem*)resize_array(
            builder->static_methods, &builder->static_method_capacity,
            sizeof(XrMethodBuildItem));
        if (builder->static_methods == NULL) return -1;
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

int xr_class_builder_add_static_method_closure(XrClassBuilder *builder,
                                                const char *name,
                                                XrClosure *closure,
                                                int param_count,
                                                uint32_t flags) {
    XR_DCHECK(builder != NULL, "add_static_method_closure: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (name == NULL) {
        xr_log_warning("class", "add_static_method_closure: invalid arguments");
        return -1;
    }

    if (builder->static_method_count >= builder->static_method_capacity) {
        builder->static_methods = (XrMethodBuildItem*)resize_array(
            builder->static_methods, &builder->static_method_capacity,
            sizeof(XrMethodBuildItem));
        if (builder->static_methods == NULL) return -1;
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

int xr_class_builder_add_interface(XrClassBuilder *builder,
                                    XrClass *interface) {
    XR_DCHECK(builder != NULL, "add_interface: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (interface == NULL) {
        xr_log_warning("class", "add_interface: invalid interface");
        return -1;
    }

    if (builder->interface_count >= builder->interface_capacity) {
        builder->interfaces = (XrClass**)resize_array(
            builder->interfaces, &builder->interface_capacity, sizeof(XrClass*));
        if (builder->interfaces == NULL) return -1;
    }

    builder->interfaces[builder->interface_count] = interface;
    builder->interface_count++;

    return 0;
}

/* ========== Abstract Method Operations ========== */

int xr_class_builder_add_abstract_method(XrClassBuilder *builder,
                                          int method_symbol) {
    XR_DCHECK(builder != NULL, "add_abstract_method: NULL builder");
    ENSURE_NOT_FINALIZED(builder);

    if (builder->abstract_method_count >= builder->abstract_method_capacity) {
        builder->abstract_methods = (int*)resize_array(
            builder->abstract_methods, &builder->abstract_method_capacity, sizeof(int));
        if (builder->abstract_methods == NULL) return -1;
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

/* ========== Instance Size Calculation ========== */

uint16_t xr_class_builder_calculate_instance_size(const XrClassBuilder *builder) {
    XR_DCHECK(builder != NULL, "calculate_instance_size: NULL builder");
    uint16_t size = sizeof(XrGCHeader);

    if (builder->super != NULL) {
        size = builder->super->instance_size;
    }

    for (int i = 0; i < builder->field_count; i++) {
        size += sizeof(void*);
    }

    return size;
}

/* ========== VTable Generation ========== */

static int find_method_in_parent_vtable(XrClass *parent_class, int symbol) {
    if (!parent_class || !parent_class->vtable) {
        return -1;
    }

    for (int i = 0; i < parent_class->vtable_size; i++) {
        if (parent_class->vtable[i] && parent_class->vtable[i]->symbol == symbol) {
            return i;
        }
    }

    return -1;
}

int xr_class_builder_generate_vtable(XrClassBuilder *builder, XrClass *cls) {
    (void)builder;

    if (cls->super && cls->super->vtable) {
        cls->vtable_size = cls->super->vtable_size;
        cls->vtable = (XrMethod**)xr_malloc(cls->vtable_size * sizeof(XrMethod*));
        if (!cls->vtable) {
            xr_log_warning("class", "generate_vtable: failed to allocate vtable");
            return -1;
        }

        memcpy(cls->vtable, cls->super->vtable, cls->vtable_size * sizeof(XrMethod*));
        cls->own_method_start = cls->super->vtable_size;
    } else {
        cls->vtable = NULL;
        cls->vtable_size = 0;
        cls->own_method_start = 0;
    }

    for (int i = 0; i < cls->method_count; i++) {
        XrMethod *method = &cls->methods[i];

        if (method->flags & XMETHOD_FLAG_STATIC) {
            method->vtable_index = -1;
            continue;
        }

        int parent_vtable_idx = find_method_in_parent_vtable(cls->super, method->symbol);

        if (parent_vtable_idx >= 0) {
            cls->vtable[parent_vtable_idx] = method;
            method->vtable_index = parent_vtable_idx;
        } else {
            cls->vtable = (XrMethod**)xr_realloc(cls->vtable,
                                                      (cls->vtable_size + 1) * sizeof(XrMethod*));
            if (!cls->vtable) {
                return -1;
            }

            cls->vtable[cls->vtable_size] = method;
            method->vtable_index = cls->vtable_size;
            cls->vtable_size++;
        }
    }

    return 0;
}

/* ========== ITable Generation ========== */
// Uses shared xr_class_build_itable() from xclass.c

/* ========== Finalize ========== */

XrClass* xr_class_builder_finalize(XrClassBuilder *builder) {
    XR_DCHECK(builder != NULL, "class_builder_finalize: NULL builder");
    if (builder == NULL) {
        xr_log_warning("class", "finalize: NULL builder");
        return NULL;
    }

    if (builder->finalized) {
        xr_log_warning("class", "finalize: already finalized");
        return NULL;
    }

    // Allocate class object from system heap (not GC-managed)
    XrClass *cls = NULL;
    if (builder->isolate && xr_isolate_get_sys_heap(builder->isolate)) {
        cls = (XrClass*)xr_sysheap_alloc_class(xr_isolate_get_sys_heap(builder->isolate), sizeof(XrClass));
    } else {
        // Fallback if system heap not initialized
        cls = (XrClass*)xr_malloc(sizeof(XrClass));
    }
    if (cls == NULL) {
        xr_log_warning("class", "finalize: failed to allocate class");
        return NULL;
    }

    memset(cls, 0, sizeof(XrClass));

    // Initialize GC header
    xr_gc_header_init_type(&cls->gc, XR_TCLASS);

    /* ========== Basic Info ========== */
    // builder->name points at an interned string owned by the symbol
    // table, so cls shares the pointer without any transfer semantics.
    cls->name = builder->name;
    cls->super = builder->super;
    cls->flags = builder->flags;

    /* ========== Primary Supers Array (JDK optimization) ========== */
    if (cls->super == NULL) {
        // Root class
        cls->depth = 0;
        cls->primary_supers[0] = cls;
        for (int i = 1; i < 8; i++) {
            cls->primary_supers[i] = NULL;
        }
    } else {
        // Inherit parent's primary supers
        cls->depth = cls->super->depth + 1;

        if (cls->depth < 8) {
            for (int i = 0; i <= cls->super->depth; i++) {
                cls->primary_supers[i] = cls->super->primary_supers[i];
            }
            cls->primary_supers[cls->depth] = cls;
            for (int i = cls->depth + 1; i < 8; i++) {
                cls->primary_supers[i] = NULL;
            }
        } else {
            // Depth >= 8: keep the 8 shallowest ancestors in
            // [Object, parent1, ..., parent7] order so that instanceof's
            // O(1) primary_supers[target->depth] lookup remains correct
            // for any target with depth < 8. Deeper targets fall back to
            // linear scan (to be replaced by secondary supers hash in P10).
            XrClass *chain[256];
            int n = 0;
            for (XrClass *c = cls; c != NULL && n < 256; c = c->super) {
                chain[n++] = c;
            }
            // chain[n-1] is Object, chain[0] is cls itself.
            for (int i = 0; i < 8 && n - 1 - i >= 0; i++) {
                cls->primary_supers[i] = chain[n - 1 - i];
            }
        }
    }

    /* ========== Field Descriptors ========== */
    // cls->fields stores only this class's declared fields
    // cls->field_count includes all inherited instance fields
    // cls->own_field_count is this class's declared field count
    int own_instance_fields = builder->field_count;
    int total_own_fields = builder->field_count + builder->static_field_count;

    // Calculate total instance fields including inherited
    int parent_instance_field_count = 0;
    if (builder->super != NULL) {
        parent_instance_field_count = xr_class_instance_field_count(builder->super);
    }
    int total_instance_field_count = parent_instance_field_count + own_instance_fields;

    if (total_own_fields > 0) {
        cls->fields = (XrFieldDescriptor*)xr_malloc(
            total_own_fields * sizeof(XrFieldDescriptor));
        if (cls->fields == NULL) {
            xr_free(cls);
            return NULL;
        }

        // Calculate instance field offset
        uint16_t offset = sizeof(XrGCHeader);
        if (builder->super != NULL) {
            offset = builder->super->instance_size;
        }

        // Add instance fields. Names are interned; sharing the pointer
        // is correct and no ownership transfer bookkeeping is required.
        for (int i = 0; i < builder->field_count; i++) {
            XrFieldBuildItem *item = &builder->fields[i];
            XrFieldDescriptor *desc = &cls->fields[i];

            desc->name = item->name;
            desc->type_name = NULL;
            desc->symbol = item->symbol;
            desc->offset = offset;
            desc->flags = item->flags;
            desc->static_slot = -1;  // Not a static field

            offset += sizeof(void*);
        }

        // Add static field descriptors
        for (int i = 0; i < builder->static_field_count; i++) {
            XrStaticFieldBuildItem *item = &builder->static_fields[i];
            XrFieldDescriptor *desc = &cls->fields[builder->field_count + i];

            desc->name = item->name;
            desc->type_name = NULL;
            desc->symbol = item->symbol;
            desc->offset = 0;
            desc->flags = item->flags | XR_FIELD_STATIC;
            desc->static_slot = (int16_t)i;  // Pre-computed static slot index
        }

        // field_count = all instance fields (inherited) + static fields
        // own_field_count = this class's declared fields
        cls->field_count = total_instance_field_count + builder->static_field_count;
        cls->own_field_count = total_own_fields;
        cls->instance_size = offset;

        // Allocate field default values array
        if (total_instance_field_count > 0) {
            cls->field_default_values = (XrValue*)xr_malloc(
                total_instance_field_count * sizeof(XrValue));
            if (cls->field_default_values != NULL) {
                for (int i = 0; i < total_instance_field_count; i++) {
                    cls->field_default_values[i] = xr_null();
                }
                for (int i = 0; i < own_instance_fields; i++) {
                    int global_idx = parent_instance_field_count + i;
                    cls->field_default_values[global_idx] = builder->fields[i].default_value;
                }
            }
        } else {
            cls->field_default_values = NULL;
        }
    } else {
        // Class without fields (e.g., Object or interface)
        cls->fields = NULL;
        cls->field_default_values = NULL;
        cls->field_count = parent_instance_field_count;
        cls->own_field_count = 0;
        cls->instance_size = (builder->super != NULL) ? builder->super->instance_size : sizeof(XrGCHeader);
    }

    // Generate field symbol-to-index map
    if (cls->own_field_count > 0) {
        int max_symbol = 0;
        for (int i = 0; i < cls->own_field_count; i++) {
            if (cls->fields[i].symbol > max_symbol) {
                max_symbol = cls->fields[i].symbol;
            }
        }

        cls->field_map_capacity = max_symbol + 1;
        cls->field_symbol_to_index = (int*)xr_malloc(cls->field_map_capacity * sizeof(int));
        if (cls->field_symbol_to_index != NULL) {
            for (int i = 0; i < cls->field_map_capacity; i++) {
                cls->field_symbol_to_index[i] = -1;
            }
            int field_base_index = parent_instance_field_count;
            for (int i = 0; i < own_instance_fields; i++) {
                int global_idx = field_base_index + i;
                cls->field_symbol_to_index[cls->fields[i].symbol] = global_idx;
            }
            // Static field mapping
            for (int i = 0; i < builder->static_field_count; i++) {
                int field_idx = own_instance_fields + i;
                cls->field_symbol_to_index[cls->fields[field_idx].symbol] = total_instance_field_count + i;
            }
        }
    }

    /* ========== Instance Methods + Static Methods (flattened) ========== */
    // Flatten parent instance methods into this class's methods[] array.
    // Layout: [inherited_methods... | own_methods... | static_methods...]
    // Override: if own method has same symbol as inherited, it replaces in-place.
    int parent_instance_method_count = 0;
    if (builder->super != NULL) {
        parent_instance_method_count = builder->super->method_count;
    }

    // Count how many own methods are overrides vs truly new
    // O(n) using parent's symbol-to-index table
    int override_count = 0;
    if (parent_instance_method_count > 0 && builder->super->method_symbol_to_index) {
        int cap = builder->super->method_map_capacity;
        for (int i = 0; i < builder->method_count; i++) {
            int sym = builder->methods[i].symbol;
            if (sym >= 0 && sym < cap && builder->super->method_symbol_to_index[sym] >= 0
                && builder->super->method_symbol_to_index[sym] < parent_instance_method_count) {
                override_count++;
            }
        }
    }

    int new_own_count = builder->method_count - override_count;
    int flat_instance_count = parent_instance_method_count + new_own_count;
    int total_method_count = flat_instance_count + builder->static_method_count;

    if (total_method_count > 0) {
        cls->methods = (XrMethod*)xr_malloc(
            total_method_count * sizeof(XrMethod));
        if (cls->methods == NULL) {
            xr_class_free(cls);
            return NULL;
        }

        // Step 1: Copy parent instance methods (shallow copy). Method
        // names are interned in the symbol table, so the child shares
        // the same pointer with no allocation or free needed.
        for (int i = 0; i < parent_instance_method_count; i++) {
            cls->methods[i] = builder->super->methods[i];
        }

        // Step 2: Apply own instance methods (override or append)
        int append_idx = parent_instance_method_count;
        for (int i = 0; i < builder->method_count; i++) {
            XrMethodBuildItem *item = &builder->methods[i];

            // Check if this overrides a parent method (O(1) via symbol table)
            int override_slot = -1;
            if (builder->super && builder->super->method_symbol_to_index
                && item->symbol >= 0
                && item->symbol < builder->super->method_map_capacity) {
                int idx = builder->super->method_symbol_to_index[item->symbol];
                if (idx >= 0 && idx < parent_instance_method_count) {
                    override_slot = idx;
                }
            }

            XrMethod *method;
            if (override_slot >= 0) {
                // Override in place. Inherited name is interned; no free.
                method = &cls->methods[override_slot];
            } else {
                method = &cls->methods[append_idx++];
            }

            method->type = item->method_type;
            switch (item->method_type) {
                case XMETHOD_PRIMITIVE:
                    method->as.primitive = item->impl.primitive;
                    break;
                case XMETHOD_CLOSURE:
                case XMETHOD_GETTER:
                case XMETHOD_SETTER:
                case XMETHOD_OPERATOR:
                    method->as.closure = item->impl.closure;
                    break;
                default:
                    method->type = XMETHOD_NONE;
                    method->as.closure = NULL;
                    break;
            }

            method->flags = item->flags & (XMETHOD_FLAG_PRIVATE | XMETHOD_FLAG_STATIC | XMETHOD_FLAG_CONSTRUCTOR | XMETHOD_FLAG_ABSTRACT | XMETHOD_FLAG_FINAL);

            method->op_type = item->op_type;
            method->symbol = item->symbol;
            method->name = item->name;
            method->param_count = item->param_count;
            method->vtable_index = -1;
        }

        cls->method_count = flat_instance_count;

        xr_class_builder_generate_vtable(builder, cls);
    }

    /* ========== Static Fields ========== */
    if (builder->static_field_count > 0) {
        cls->static_field_values = (XrValue*)xr_malloc(
            builder->static_field_count * sizeof(XrValue));
        if (cls->static_field_values == NULL) {
            xr_class_free(cls);
            return NULL;
        }

        for (int i = 0; i < builder->static_field_count; i++) {
            cls->static_field_values[i] = builder->static_fields[i].value;
        }

        cls->static_field_count = builder->static_field_count;
    }

    /* ========== Static Methods ========== */
    // Methods array already allocated above with total_method_count capacity
    if (builder->static_method_count > 0) {
        for (int i = 0; i < builder->static_method_count; i++) {
            XrMethodBuildItem *item = &builder->static_methods[i];
            XrMethod *method = &cls->methods[flat_instance_count + i];

            method->type = item->method_type;
            switch (item->method_type) {
                case XMETHOD_PRIMITIVE:
                    method->as.primitive = item->impl.primitive;
                    break;
                case XMETHOD_CLOSURE:
                case XMETHOD_GETTER:
                case XMETHOD_SETTER:
                case XMETHOD_OPERATOR:
                    method->as.closure = item->impl.closure;
                    break;
                default:
                    method->type = XMETHOD_NONE;
                    method->as.closure = NULL;
                    break;
            }

            method->flags = (item->flags & (XMETHOD_FLAG_PRIVATE | XMETHOD_FLAG_ABSTRACT | XMETHOD_FLAG_FINAL)) | XMETHOD_FLAG_STATIC;

            method->op_type = item->op_type;
            method->symbol = item->symbol;
            method->name = item->name;
            method->param_count = item->param_count;
            method->vtable_index = -1;
        }

        cls->method_count = total_method_count;
        cls->static_method_count = builder->static_method_count;
    }

    /* ========== Interfaces ========== */
    if (builder->interface_count > 0) {
        cls->interfaces = (XrClass**)xr_malloc(
            builder->interface_count * sizeof(XrClass*));
        if (cls->interfaces != NULL) {
            memcpy(cls->interfaces, builder->interfaces,
                   builder->interface_count * sizeof(XrClass*));
            cls->interface_count = builder->interface_count;
        }
    }

    /* ========== Abstract Methods ========== */
    if (builder->abstract_method_count > 0) {
        cls->abstract_methods = (int*)xr_malloc(
            builder->abstract_method_count * sizeof(int));
        if (cls->abstract_methods != NULL) {
            memcpy(cls->abstract_methods, builder->abstract_methods,
                   builder->abstract_method_count * sizeof(int));
            cls->abstract_method_count = builder->abstract_method_count;
        }
    }

    xr_class_build_itable(cls);

    // Generate method symbol-to-index map
    if (cls->method_count > 0) {
        int max_symbol = 0;
        for (int i = 0; i < cls->method_count; i++) {
            if (cls->methods[i].symbol > max_symbol) {
                max_symbol = cls->methods[i].symbol;
            }
        }

        cls->method_map_capacity = max_symbol + 1;
        cls->method_symbol_to_index = (int*)xr_malloc(
            cls->method_map_capacity * sizeof(int)
        );

        if (cls->method_symbol_to_index != NULL) {
            for (int i = 0; i < cls->method_map_capacity; i++) {
                cls->method_symbol_to_index[i] = -1;
            }

            for (int i = 0; i < cls->method_count; i++) {
                int symbol = cls->methods[i].symbol;
                if (symbol >= 0 && symbol < cls->method_map_capacity) {
                    cls->method_symbol_to_index[symbol] = i;
                }
            }
        }
    }

    xr_class_compute_operator_flags(cls);

    builder->finalized = true;
    xr_class_builder_destroy(builder);

    return cls;
}

/* ========== Builder Destroy ========== */

void xr_class_builder_destroy(XrClassBuilder *builder) {
    if (builder == NULL) return;

    // All name fields below point into the symbol table's intern pool and
    // must not be freed here; only the dynamic arrays themselves are ours.
    xr_free(builder->fields);
    xr_free(builder->methods);
    xr_free(builder->static_fields);
    xr_free(builder->static_methods);
    xr_free(builder->interfaces);
    xr_free(builder->abstract_methods);
    xr_free(builder);
}
