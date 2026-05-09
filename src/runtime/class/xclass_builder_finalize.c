/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_builder_finalize.c - Terminal stage of class construction
 *
 * KEY CONCEPT:
 *   xr_class_builder_finalize used to live in xclass_builder.c as a
 *   single ~470-line function. The body is split into a dispatcher
 *   and a handful of static finalize_* helpers here; the rest of
 *   xclass_builder.c only carries builder lifecycle and the add_*
 *   APIs. Having the finalize pipeline in its own translation unit
 *   also lets generate_vtable / find_method_in_parent_vtable drop
 *   back to file-local linkage (they had no external callers).
 *
 * RELATED MODULES:
 *   - xclass_builder.c / .h  : Builder lifecycle + add_* / has_*
 *   - xclass_builder_internal.h : XrClassBuilder layout
 *   - xclass_internal.h      : xr_class_free, build_itable,
 *                               compute_operator_flags
 */

#include "xclass.h"
#include "xclass_internal.h"
#include "xclass_builder.h"
#include "xclass_builder_internal.h"
#include "xreflect_cache.h"
#include "xreflect_registry.h"
#include "xmethod.h"
#include "../xisolate_api.h"
#include "../../base/xmalloc.h"
#include "../../base/xlog.h"
#include "../../base/xchecks.h"
#include "../../base/xhash.h"
#include "../gc/xgc_header.h"
#include "../gc/xsystem_heap.h"
#include <string.h>

/* ========== VTable Generation (file-local) ========== */

// Locate the parent's vtable slot for `symbol` in O(1): the parent's
// method_symbol_to_index maps a symbol to its slot in
// parent->methods[], and each method already caches its vtable slot in
// method->vtable_index.
static int find_method_in_parent_vtable(XrClass *parent_class, int symbol) {
    if (!parent_class || !parent_class->vtable || symbol < 0) {
        return -1;
    }
    if (!parent_class->method_symbol_to_index || symbol >= parent_class->method_map_capacity) {
        return -1;
    }
    int method_idx = parent_class->method_symbol_to_index[symbol];
    if (method_idx < 0 || method_idx >= parent_class->method_count) {
        return -1;
    }
    int vtable_idx = parent_class->methods[method_idx].vtable_index;
    if (vtable_idx < 0 || vtable_idx >= parent_class->vtable_size) {
        return -1;
    }
    return vtable_idx;
}

// Populate cls->vtable from cls->methods[]. Returns 0 on success,
// -1 on allocation failure; on failure the partially-built vtable
// may leak, but finalize catches the error and routes through
// xr_class_free so nothing reaches cls's observable lifetime.
static int generate_vtable(XrClass *cls) {
    if (cls->super && cls->super->vtable) {
        cls->vtable_size = cls->super->vtable_size;
        cls->vtable = (XrMethod **) xr_malloc(cls->vtable_size * sizeof(XrMethod *));
        if (!cls->vtable) {
            xr_log_warning("class", "generate_vtable: failed to allocate vtable");
            return -1;
        }
        memcpy(cls->vtable, cls->super->vtable, cls->vtable_size * sizeof(XrMethod *));
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
            // Route realloc through a temporary so a failed grow does
            // not overwrite cls->vtable with NULL and leak the previous
            // buffer.
            XrMethod **new_vtable =
                (XrMethod **) xr_realloc(cls->vtable, (cls->vtable_size + 1) * sizeof(XrMethod *));
            if (!new_vtable) {
                return -1;
            }
            cls->vtable = new_vtable;
            cls->vtable[cls->vtable_size] = method;
            method->vtable_index = cls->vtable_size;
            cls->vtable_size++;
        }
    }
    return 0;
}

/* ========== Finalize Helpers (forward decls) ========== */

static void finalize_basic_and_supers(const XrClassBuilder *b, XrClass *cls);
static bool finalize_fields(const XrClassBuilder *b, XrClass *cls, int parent_instance_field_count,
                            int own_instance_fields, int total_own_fields,
                            int total_instance_field_count);
static void finalize_field_symbol_map(const XrClassBuilder *b, XrClass *cls,
                                      int parent_instance_field_count, int own_instance_fields,
                                      int total_instance_field_count);
static bool finalize_methods(XrClassBuilder *b, XrClass *cls, int parent_instance_method_count,
                             int flat_instance_count, int total_method_count);
static bool finalize_static_fields(const XrClassBuilder *b, XrClass *cls);
static void finalize_static_methods(const XrClassBuilder *b, XrClass *cls, int flat_instance_count,
                                    int total_method_count);
static void finalize_interfaces(const XrClassBuilder *b, XrClass *cls);
static void finalize_abstract_methods(const XrClassBuilder *b, XrClass *cls);
static void finalize_method_symbol_map(XrClass *cls);
static void finalize_eager_reflection(XrClassBuilder *b, XrClass *cls);
static void write_method_slot(XrMethod *method, XrMethodBuildItem *item, bool is_static);

/* ========== Finalize (top-level dispatcher) ========== */

XrClass *xr_class_builder_finalize(XrClassBuilder *builder) {
    XR_DCHECK(builder != NULL, "class_builder_finalize: NULL builder");
    if (builder == NULL) {
        xr_log_warning("class", "finalize: NULL builder");
        return NULL;
    }
    if (builder->finalized) {
        xr_log_warning("class", "finalize: already finalized");
        return NULL;
    }

    // Allocate class object. Uses the isolate-owned system heap when
    // available so classes share a cache-friendly pool; otherwise falls
    // back to the generic allocator (covers early bootstrap before the
    // sys-heap is wired up).
    XrClass *cls = NULL;
    if (builder->isolate && xr_isolate_get_sys_heap(builder->isolate)) {
        cls = (XrClass *) xr_sysheap_alloc_class(xr_isolate_get_sys_heap(builder->isolate),
                                                 sizeof(XrClass));
    } else {
        cls = (XrClass *) xr_malloc(sizeof(XrClass));
    }
    if (cls == NULL) {
        xr_log_warning("class", "finalize: failed to allocate class");
        return NULL;
    }
    memset(cls, 0, sizeof(XrClass));
    xr_gc_header_init_type(&cls->gc, XR_TCLASS);

    finalize_basic_and_supers(builder, cls);

    // Field counts shared by finalize_fields and finalize_field_symbol_map.
    int own_instance_fields = builder->field_count;
    int total_own_fields = builder->field_count + builder->static_field_count;
    int parent_instance_field_count =
        (builder->super != NULL) ? xr_class_instance_field_count(builder->super) : 0;
    int total_instance_field_count = parent_instance_field_count + own_instance_fields;

    if (!finalize_fields(builder, cls, parent_instance_field_count, own_instance_fields,
                         total_own_fields, total_instance_field_count)) {
        xr_class_free(cls);
        return NULL;
    }
    finalize_field_symbol_map(builder, cls, parent_instance_field_count, own_instance_fields,
                              total_instance_field_count);

    // Method counts shared by finalize_methods / finalize_static_methods.
    int parent_instance_method_count = (builder->super != NULL) ? builder->super->method_count : 0;

    int override_count = 0;
    if (parent_instance_method_count > 0 && builder->super->method_symbol_to_index) {
        int cap = builder->super->method_map_capacity;
        for (int i = 0; i < builder->method_count; i++) {
            int sym = builder->methods[i].symbol;
            if (sym >= 0 && sym < cap && builder->super->method_symbol_to_index[sym] >= 0 &&
                builder->super->method_symbol_to_index[sym] < parent_instance_method_count) {
                override_count++;
            }
        }
    }
    int flat_instance_count =
        parent_instance_method_count + (builder->method_count - override_count);
    int total_method_count = flat_instance_count + builder->static_method_count;

    if (!finalize_methods(builder, cls, parent_instance_method_count, flat_instance_count,
                          total_method_count)) {
        xr_class_free(cls);
        return NULL;
    }
    if (!finalize_static_fields(builder, cls)) {
        xr_class_free(cls);
        return NULL;
    }
    finalize_static_methods(builder, cls, flat_instance_count, total_method_count);
    finalize_interfaces(builder, cls);
    finalize_abstract_methods(builder, cls);
    finalize_method_symbol_map(cls);

    xr_class_build_itable(cls);
    xr_class_compute_operator_flags(cls);

    finalize_eager_reflection(builder, cls);

    builder->finalized = true;
    xr_class_builder_destroy(builder);

    return cls;
}

/* ========== Finalize Helper Implementations ========== */

// Basic immutable info (name / super / flags) plus the primary_supers
// array and the optional secondary_supers_hash for depth >= 8 chains.
// See xr_class_instanceof for how these cooperate to deliver O(1)
// subtype checks in the common (depth < 8) case and O(1) hash probes
// in the deep-inheritance case.
static void finalize_basic_and_supers(const XrClassBuilder *b, XrClass *cls) {
    // builder->name is a symbol-table-interned pointer; sharing it with
    // cls carries no ownership transfer.
    cls->name = b->name;
    cls->display_name = b->display_name;
    cls->super = b->super;
    cls->generic_origin = b->generic_origin;
    cls->flags = b->flags;

    /* Transfer mono_type_arg_names ownership: builder allocated the copy,
     * class takes it; builder's pointer is NULLed to prevent double-free
     * in xr_class_builder_destroy. */
    cls->mono_type_argc = b->mono_type_argc;
    cls->mono_type_arg_names = b->mono_type_arg_names;
    ((XrClassBuilder *) b)->mono_type_arg_names = NULL;
    ((XrClassBuilder *) b)->mono_type_argc = 0;

    if (cls->super == NULL) {
        cls->depth = 0;
        cls->primary_supers[0] = cls;
        for (int i = 1; i < 8; i++)
            cls->primary_supers[i] = NULL;
        return;
    }

    cls->depth = cls->super->depth + 1;
    if (cls->depth < 8) {
        for (int i = 0; i <= cls->super->depth; i++) {
            cls->primary_supers[i] = cls->super->primary_supers[i];
        }
        cls->primary_supers[cls->depth] = cls;
        for (int i = cls->depth + 1; i < 8; i++)
            cls->primary_supers[i] = NULL;
        return;
    }

    // Depth >= 8: keep the 8 shallowest ancestors in
    // [Object, parent1, ..., parent7] order for primary_supers and
    // then build a secondary-supers hash keyed by ancestor identity.
    XrClass *chain[256];
    int n = 0;
    for (XrClass *c = cls; c != NULL && n < 256; c = c->super)
        chain[n++] = c;
    for (int i = 0; i < 8 && n - 1 - i >= 0; i++) {
        cls->primary_supers[i] = chain[n - 1 - i];
    }

    // Load factor <= 0.5 so the linear-probe chain stays short. A
    // failed allocation is non-fatal: instanceof falls back to a
    // linear super-chain walk when secondary_supers_hash is NULL.
    uint16_t cap = 16;
    while (cap < (uint16_t) (n * 2))
        cap <<= 1;
    cls->secondary_supers_hash = (XrClass **) xr_calloc(cap, sizeof(XrClass *));
    if (cls->secondary_supers_hash == NULL)
        return;

    cls->secondary_supers_capacity = cap;
    uint32_t mask = (uint32_t) cap - 1;
    for (int i = 0; i < n; i++) {
        XrClass *c = chain[i];
        uint32_t h = xr_hash_int((int64_t) (uintptr_t) c) & mask;
        while (cls->secondary_supers_hash[h] != NULL)
            h = (h + 1) & mask;
        cls->secondary_supers_hash[h] = c;
    }
}

// Populate cls->fields (instance + static descriptors), cls->instance_size,
// and cls->field_default_values. Returns false on cls->fields alloc
// failure (the only hard error in this stage).
static bool finalize_fields(const XrClassBuilder *b, XrClass *cls, int parent_instance_field_count,
                            int own_instance_fields, int total_own_fields,
                            int total_instance_field_count) {
    if (total_own_fields == 0) {
        cls->fields = NULL;
        cls->field_default_values = NULL;
        cls->field_count = parent_instance_field_count;
        cls->own_field_count = 0;
        cls->instance_size = (b->super != NULL) ? b->super->instance_size : sizeof(XrGCHeader);
        return true;
    }

    cls->fields = (XrFieldDescriptor *) xr_malloc(total_own_fields * sizeof(XrFieldDescriptor));
    if (cls->fields == NULL)
        return false;

    // Instance fields go first, at offsets that continue the parent's
    // instance layout so inherited accesses stay a plain offset read.
    uint16_t offset = (b->super != NULL) ? b->super->instance_size : (uint16_t) sizeof(XrGCHeader);
    for (int i = 0; i < b->field_count; i++) {
        XrFieldBuildItem *item = &b->fields[i];
        XrFieldDescriptor *desc = &cls->fields[i];
        desc->name = item->name;  // interned, shared
        desc->type_name = NULL;
        desc->symbol = item->symbol;
        desc->offset = offset;
        desc->flags = item->flags;
        desc->static_slot = -1;
        offset += sizeof(void *);
    }
    // Static fields share cls->fields but carry XR_FIELD_STATIC and a
    // pre-computed static_slot pointing into cls->static_field_values.
    for (int i = 0; i < b->static_field_count; i++) {
        XrStaticFieldBuildItem *item = &b->static_fields[i];
        XrFieldDescriptor *desc = &cls->fields[b->field_count + i];
        desc->name = item->name;
        desc->type_name = NULL;
        desc->symbol = item->symbol;
        desc->offset = 0;
        desc->flags = item->flags | XR_FIELD_STATIC;
        desc->static_slot = (int16_t) i;
    }

    cls->field_count = total_instance_field_count + b->static_field_count;
    cls->own_field_count = total_own_fields;
    cls->instance_size = offset;

    if (total_instance_field_count > 0) {
        cls->field_default_values =
            (XrValue *) xr_malloc(total_instance_field_count * sizeof(XrValue));
        if (cls->field_default_values != NULL) {
            for (int i = 0; i < total_instance_field_count; i++) {
                cls->field_default_values[i] = xr_null();
            }
            for (int i = 0; i < own_instance_fields; i++) {
                int global_idx = parent_instance_field_count + i;
                cls->field_default_values[global_idx] = b->fields[i].default_value;
            }
        }
    }
    return true;
}

// symbol -> field index map. Best-effort (xr_class_lookup_field walks
// the super chain recursively if this map is missing).
static void finalize_field_symbol_map(const XrClassBuilder *b, XrClass *cls,
                                      int parent_instance_field_count, int own_instance_fields,
                                      int total_instance_field_count) {
    if (cls->own_field_count == 0)
        return;

    int max_symbol = 0;
    for (int i = 0; i < cls->own_field_count; i++) {
        if (cls->fields[i].symbol > max_symbol)
            max_symbol = cls->fields[i].symbol;
    }
    cls->field_map_capacity = max_symbol + 1;
    cls->field_symbol_to_index = (int *) xr_malloc(cls->field_map_capacity * sizeof(int));
    if (cls->field_symbol_to_index == NULL)
        return;

    for (int i = 0; i < cls->field_map_capacity; i++) {
        cls->field_symbol_to_index[i] = -1;
    }
    for (int i = 0; i < own_instance_fields; i++) {
        int global_idx = parent_instance_field_count + i;
        cls->field_symbol_to_index[cls->fields[i].symbol] = global_idx;
    }
    for (int i = 0; i < b->static_field_count; i++) {
        int field_idx = own_instance_fields + i;
        cls->field_symbol_to_index[cls->fields[field_idx].symbol] = total_instance_field_count + i;
    }
}

// Copy one XrMethodBuildItem into an XrMethod slot. Keeps the dense
// switch from the old finalize body in a single place so the two
// call sites (instance methods and static methods) stay in sync.
static void write_method_slot(XrMethod *method, XrMethodBuildItem *item, bool is_static) {
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

    uint32_t flag_mask =
        is_static ? (XMETHOD_FLAG_PRIVATE | XMETHOD_FLAG_ABSTRACT | XMETHOD_FLAG_FINAL)
                  : (XMETHOD_FLAG_PRIVATE | XMETHOD_FLAG_STATIC | XMETHOD_FLAG_CONSTRUCTOR |
                     XMETHOD_FLAG_ABSTRACT | XMETHOD_FLAG_FINAL);
    method->flags = item->flags & flag_mask;
    if (is_static)
        method->flags |= XMETHOD_FLAG_STATIC;

    method->op_type = item->op_type;
    method->symbol = item->symbol;
    method->name = item->name;
    method->param_count = item->param_count;
    method->vtable_index = -1;
}

// Flatten parent instance methods into cls->methods[], apply the
// class's own methods (override-in-place when the symbol matches an
// inherited slot), then run vtable generation. Returns false on any
// allocation failure.
static bool finalize_methods(XrClassBuilder *b, XrClass *cls, int parent_instance_method_count,
                             int flat_instance_count, int total_method_count) {
    if (total_method_count == 0)
        return true;

    cls->methods = (XrMethod *) xr_malloc(total_method_count * sizeof(XrMethod));
    if (cls->methods == NULL)
        return false;

    // Inherited instance methods shallow-copied; names are interned so
    // sharing the pointer is correct without any transfer bookkeeping.
    for (int i = 0; i < parent_instance_method_count; i++) {
        cls->methods[i] = b->super->methods[i];
    }

    int append_idx = parent_instance_method_count;
    for (int i = 0; i < b->method_count; i++) {
        XrMethodBuildItem *item = &b->methods[i];
        int override_slot = -1;
        if (b->super && b->super->method_symbol_to_index && item->symbol >= 0 &&
            item->symbol < b->super->method_map_capacity) {
            int idx = b->super->method_symbol_to_index[item->symbol];
            if (idx >= 0 && idx < parent_instance_method_count)
                override_slot = idx;
        }
        XrMethod *method =
            (override_slot >= 0) ? &cls->methods[override_slot] : &cls->methods[append_idx++];
        write_method_slot(method, item, /*is_static=*/false);
    }
    cls->method_count = flat_instance_count;

    // vtable generation is strict: OOM rolls the class back. Leaving a
    // class without a vtable would silently break dispatch later.
    return generate_vtable(cls) == 0;
}

// Static fields: separate storage from instance fields. Returns false
// on the only hard failure (alloc).
static bool finalize_static_fields(const XrClassBuilder *b, XrClass *cls) {
    if (b->static_field_count == 0)
        return true;

    cls->static_field_values = (XrValue *) xr_malloc(b->static_field_count * sizeof(XrValue));
    if (cls->static_field_values == NULL)
        return false;

    for (int i = 0; i < b->static_field_count; i++) {
        cls->static_field_values[i] = b->static_fields[i].value;
    }
    cls->static_field_count = b->static_field_count;
    return true;
}

// Append static methods to the already-sized cls->methods[] (instance
// capacity allocated by finalize_methods).
static void finalize_static_methods(const XrClassBuilder *b, XrClass *cls, int flat_instance_count,
                                    int total_method_count) {
    if (b->static_method_count == 0)
        return;

    for (int i = 0; i < b->static_method_count; i++) {
        XrMethod *method = &cls->methods[flat_instance_count + i];
        write_method_slot(method, &b->static_methods[i], /*is_static=*/true);
    }
    cls->method_count = total_method_count;
    cls->static_method_count = b->static_method_count;
}

// Interface pointer list. Allocation failure leaves cls->interfaces
// NULL + interface_count == 0; implements_interface handles that
// edge gracefully by returning false.
static void finalize_interfaces(const XrClassBuilder *b, XrClass *cls) {
    if (b->interface_count == 0)
        return;

    cls->interfaces = (XrClass **) xr_malloc(b->interface_count * sizeof(XrClass *));
    if (cls->interfaces == NULL)
        return;
    memcpy(cls->interfaces, b->interfaces, b->interface_count * sizeof(XrClass *));
    cls->interface_count = b->interface_count;
}

// Abstract method symbol list. Same best-effort semantics as
// interfaces -- consumers (xr_class_can_instantiate etc.) treat a
// NULL abstract_methods + count == 0 as "no abstract methods".
static void finalize_abstract_methods(const XrClassBuilder *b, XrClass *cls) {
    if (b->abstract_method_count == 0)
        return;

    cls->abstract_methods = (int *) xr_malloc(b->abstract_method_count * sizeof(int));
    if (cls->abstract_methods == NULL)
        return;
    memcpy(cls->abstract_methods, b->abstract_methods, b->abstract_method_count * sizeof(int));
    cls->abstract_method_count = b->abstract_method_count;
}

// symbol -> method index map. Built BEFORE xr_class_build_itable so
// the itable builder can do O(1) interface method -> implementation
// lookups via cls->method_symbol_to_index instead of a linear scan
// through cls->methods[].
static void finalize_method_symbol_map(XrClass *cls) {
    if (cls->method_count == 0)
        return;

    int max_symbol = 0;
    for (int i = 0; i < cls->method_count; i++) {
        if (cls->methods[i].symbol > max_symbol)
            max_symbol = cls->methods[i].symbol;
    }
    cls->method_map_capacity = max_symbol + 1;
    cls->method_symbol_to_index = (int *) xr_malloc(cls->method_map_capacity * sizeof(int));
    if (cls->method_symbol_to_index == NULL)
        return;

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

// Eagerly build reflect_cache + register type_metadata. Both outputs
// are mandatory for any class that reaches the end of finalize, which
// is why no caller anywhere else in the tree needs to re-register a
// freshly created class. Allocation failures are logged but
// non-fatal: reader sites tolerate a NULL cache or a missing registry
// entry and behave as if the old lazy path were still active.
static void finalize_eager_reflection(XrClassBuilder *b, XrClass *cls) {
    if (b->isolate == NULL)
        return;

    if (cls->reflect_cache == NULL) {
        XrReflectCache *rcache = xr_reflect_cache_create(b->isolate, cls);
        if (rcache != NULL) {
            cls->reflect_cache = rcache;
        } else {
            xr_log_warning("class", "finalize: reflect_cache allocation failed for '%s'",
                           cls->name ? cls->name : "<anonymous>");
        }
    }
    if (cls->type_metadata == NULL && xr_isolate_get_type_registry(b->isolate) != NULL) {
        (void) xr_registry_register_class(b->isolate, cls);
    }
}
