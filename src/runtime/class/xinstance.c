/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xinstance.c - Instance object implementation
 */

#include "xinstance.h"
#include "../xisolate_api.h"
#include "../xisolate_internal.h"
#include "xclass_system.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../../shared/xr_float_fmt.h"
#include "xclass.h"
#include "xmethod.h"
#include "../../base/xmalloc.h"
#include "../mem/xheap.h"
#include "../mem/xalloc_unified.h"
#include "../core/xr_runtime_core.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../xisolate_api.h"
#include "../xvm_call.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ========== Instance Operations ========== */

// Access control handled by compiler/interpreter
XrValue xr_instance_get_field(XrVMRuntime *X, XrInstance *inst, const char *name) {
    if (!X || !inst || !name)
        return xr_null();

    XrClass *klass = xr_instance_get_class(inst);
    if (!klass)
        return xr_null();

    int index = xr_class_lookup_field_by_name(X, klass, name);
    if (index < 0) {
        xr_log_warning("instance", "field '%s' not found in class '%s'", name,
                       xr_class_display_name(klass));
        return xr_null();
    }

    // Bounds check: ensure index is within instance field range
    int ifc = xr_class_instance_field_count(klass);
    if (index >= ifc) {
        xr_log_warning("instance", "field index %d out of bounds (max %d)", index, ifc - 1);
        return xr_null();
    }

    return inst->fields[index];
}

void xr_instance_set_field(XrVMRuntime *X, XrInstance *inst, const char *name, XrValue value) {
    if (!X || !inst || !name)
        return;

    XrClass *klass = xr_instance_get_class(inst);
    if (!klass)
        return;

    int index = xr_class_lookup_field_by_name(X, klass, name);
    if (index < 0) {
        xr_log_warning("instance", "field '%s' not found in class '%s'", name,
                       xr_class_display_name(klass));
        return;
    }

    // Bounds check: ensure index is within instance field range
    int ifc = xr_class_instance_field_count(klass);
    if (index >= ifc) {
        xr_log_warning("instance", "field index %d out of bounds (max %d)", index, ifc - 1);
        return;
    }

    xr_rc_release_value(xr_current_coro_heap(), inst->fields[index]);
    inst->fields[index] = value;
}

XrValue xr_instance_get_field_by_index(XrInstance *inst, int index) {
    XR_DCHECK(inst != NULL, "Instance must not be NULL");
    XrClass *klass = xr_instance_get_class(inst);
    XR_DCHECK_BOUNDS(index, klass->field_count, "field index out of bounds");
    (void) klass;
    return inst->fields[index];
}

void xr_instance_set_field_by_index(XrInstance *inst, int index, XrValue value) {
    XR_DCHECK(inst != NULL, "Instance must not be NULL");
    XrClass *klass = xr_instance_get_class(inst);
    XR_DCHECK_BOUNDS(index, klass->field_count, "field index out of bounds");
    (void) klass;
    xr_rc_release_value(xr_current_coro_heap(), inst->fields[index]);
    inst->fields[index] = value;
}

XrValue xr_instance_call_method(XrVMRuntime *X, XrInstance *inst, const char *name, XrValue *args,
                                int argc) {
    if (!X || !inst || !name)
        return xr_null();

    XrClass *klass = xr_instance_get_class(inst);
    if (!klass)
        return xr_null();

    // Convert method name to symbol
    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
    if (!sym_table) {
        xr_log_warning("instance", "symbol table not available");
        return xr_null();
    }

    SymbolId method_symbol = xr_symbol_lookup_in_table(sym_table, name);
    if (method_symbol == SYMBOL_INVALID) {
        xr_log_warning("instance", "method '%s' not found in class '%s'", name,
                       xr_class_display_name(klass));
        return xr_null();
    }

    XrMethod *method = xr_class_lookup_method(klass, method_symbol);
    if (!method) {
        xr_log_warning("instance", "method '%s' not found in class '%s'", name,
                       xr_class_display_name(klass));
        return xr_null();
    }

    XrValue this_value = xr_value_from_instance(inst);

    // Call method based on type
    XrValue result = xr_null();
    if (method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
        result = method->as.primitive(X, this_value, args, argc);
    } else if (method->as.closure) {
        // Closure calling convention still uses args[0]=self
        XrValue stack_buf[9];
        XrValue *full_args =
            (argc + 1 <= 9) ? stack_buf : (XrValue *) xr_malloc(sizeof(XrValue) * (argc + 1));
        if (!full_args) {
            xr_log_warning("instance", "failed to allocate argument array");
            return xr_null();
        }
        full_args[0] = this_value;
        for (int i = 0; i < argc; i++) {
            full_args[i + 1] = args[i];
        }
        result = xr_vm_call_closure(X, method->as.closure, full_args, argc + 1);
        if (full_args != stack_buf)
            xr_free(full_args);
    }

    return result;
}

/* ========== Debug ========== */

void xr_instance_print(XrInstance *inst) {
    if (!inst) {
        printf("null instance\n");
        return;
    }

    XrClass *klass = xr_instance_get_class(inst);
    if (!klass) {
        printf("<invalid instance>\n");
        return;
    }

    const char *class_name = xr_class_display_name(klass);
    printf("%s instance {\n", class_name);

    // Use instance field count (exclude static fields)
    int ifc = xr_class_instance_field_count(klass);

    for (int i = 0; i < ifc; i++) {
        const char *field_name = (klass->fields && i < klass->field_count && klass->fields[i].name)
                                     ? klass->fields[i].name
                                     : "unknown";
        printf("  %s: ", field_name);

        XrValue val = inst->fields[i];
        if (XR_IS_NULL(val)) {
            printf("null");
        } else if (XR_IS_BOOL(val)) {
            printf("%s", XR_TO_BOOL(val) ? "true" : "false");
        } else if (XR_IS_INT(val)) {
            printf("%lld", XR_TO_INT(val));
        } else if (XR_IS_FLOAT(val)) {
            char _fb[64];
            xr_format_float(_fb, sizeof(_fb), XR_TO_FLOAT(val));
            printf("%s", _fb);
        } else if (XR_IS_STRING(val)) {
            XrString *str = XR_TO_STRING(val);
            printf("\"%s\"", str ? str->data : "<null>");
        } else {
            printf("<object>");
        }
        printf("\n");
    }
    printf("}\n");
}

bool xr_instance_is_a(XrInstance *inst, XrClass *cls) {
    if (!inst || !cls)
        return false;
    return xr_class_instanceof(xr_instance_get_class(inst), cls);
}

/* ========== Class Transition ========== */

static XrClass *xr_class_transition_get_or_create_impl(XrVMRuntime *X, XrClass *klass, int symbol,
                                                       const char *field_name,
                                                       bool allow_sealed_source) {
    XR_DCHECK(X != NULL, "transition: NULL isolate");
    XR_DCHECK(klass != NULL, "transition: NULL klass");
    XR_DCHECK(klass->flags & XR_CLASS_DYNAMIC_LAYOUT, "transition: not dynamic");
    (void) X;

    // Runtime field additions must reject sealed classes before consulting the
    // transition cache. Compile-time class-chain construction intentionally
    // bypasses this so wider sealed Record shapes can reuse sealed prefixes.
    if (!allow_sealed_source && (klass->flags & XR_CLASS_DYNAMIC_SEALED)) {
        return NULL;
    }

    // Fast path: lock-free search. XrClass is isolate-shared metadata and the
    // transition list only ever grows with immortal, immutable nodes published
    // via a release-store on the head. An acquire-load lets concurrent worker
    // threads traverse safely without touching a lock, so dynamic field access
    // stays lock-free on the hot path (P1-3).
    for (XrClassTransition *t = atomic_load_explicit(&klass->transitions, memory_order_acquire); t;
         t = t->next) {
        if (t->symbol == symbol)
            return t->target;
    }

    // Slow path: serialize creation so two workers adding the same field to the
    // same shape cannot fork the transition chain (which would break shape
    // identity for inline caches / instanceof).
    XrRuntimeCore *core = xr_isolate_get_runtime_core(X);
    if (core)
        xr_amutex_lock(&core->metadata_lock);

    // Double-check under the lock: another worker may have installed this exact
    // transition between our lock-free miss and acquiring the lock.
    for (XrClassTransition *t = atomic_load_explicit(&klass->transitions, memory_order_acquire); t;
         t = t->next) {
        if (t->symbol == symbol) {
            if (core)
                xr_amutex_unlock(&core->metadata_lock);
            return t->target;
        }
    }

    // Create child class: inherits all parent fields + one new field
    uint16_t parent_fc = klass->field_count;
    uint16_t child_fc = parent_fc + 1;

    XrClass *child = (XrClass *) xr_calloc(1, sizeof(XrClass));
    if (!child) {
        if (core)
            xr_amutex_unlock(&core->metadata_lock);
        return NULL;
    }

    // Copy key fields from parent. The all-zero XrObjHeader from xr_calloc
    // intentionally matches xr_class_new_dynamic_root: dynamic-layout shapes are
    // permanent metadata identified by XR_CLASS_DYNAMIC_LAYOUT + builtin_kind,
    // not by hdr.type, and are never GC-managed or freed.
    child->name = klass->name;
    child->super = klass->super;
    child->flags = klass->flags;
    child->builtin_kind = klass->builtin_kind;
    child->in_object_capacity = klass->in_object_capacity;
    child->transition_parent = klass;
    child->transition_symbol = symbol;
    atomic_store_explicit(&child->transitions, NULL, memory_order_relaxed);

    // Build field descriptor array: parent fields + new field
    child->fields = (XrFieldDescriptor *) xr_malloc(sizeof(XrFieldDescriptor) * child_fc);
    if (!child->fields) {
        xr_free(child);
        if (core)
            xr_amutex_unlock(&core->metadata_lock);
        return NULL;
    }
    if (parent_fc > 0 && klass->fields) {
        memcpy(child->fields, klass->fields, sizeof(XrFieldDescriptor) * parent_fc);
    }
    // New field descriptor
    XrFieldDescriptor *new_fd = &child->fields[parent_fc];
    memset(new_fd, 0, sizeof(XrFieldDescriptor));
    new_fd->name = field_name;
    new_fd->symbol = symbol;
    new_fd->offset = parent_fc;

    child->field_count = child_fc;
    child->own_field_count = klass->own_field_count + 1;

    // field_symbol_to_index is direct-indexed by symbol id, so capacity
    // must cover (max_symbol + 1). Grow when the new field's symbol
    // exceeds the parent's capacity.
    int new_cap = klass->field_map_capacity;
    if (symbol + 1 > new_cap)
        new_cap = symbol + 1;
    child->field_symbol_to_index = (int *) xr_malloc(sizeof(int) * new_cap);
    if (!child->field_symbol_to_index) {
        xr_free(child->fields);
        xr_free(child);
        if (core)
            xr_amutex_unlock(&core->metadata_lock);
        return NULL;
    }
    for (int i = 0; i < new_cap; i++)
        child->field_symbol_to_index[i] = -1;
    child->field_map_capacity = new_cap;

    for (uint16_t i = 0; i < child_fc; i++) {
        int s = child->fields[i].symbol;
        if (s >= 0 && s < new_cap)
            child->field_symbol_to_index[s] = i;
    }

    // Register transition on parent.
    XrClassTransition *trans = (XrClassTransition *) xr_malloc(sizeof(XrClassTransition));
    if (!trans) {
        xr_free(child->field_symbol_to_index);
        xr_free(child->fields);
        xr_free(child);
        if (core)
            xr_amutex_unlock(&core->metadata_lock);
        return NULL;
    }
    trans->symbol = symbol;
    trans->target = child;
    trans->next = atomic_load_explicit(&klass->transitions, memory_order_relaxed);
    // Publish with release: any reader that acquire-loads the new head is
    // guaranteed to see the fully constructed `child` and `trans->next` chain.
    atomic_store_explicit(&klass->transitions, trans, memory_order_release);

    if (core)
        xr_amutex_unlock(&core->metadata_lock);
    return child;
}

XrClass *xr_class_transition_get_or_create(XrVMRuntime *X, XrClass *klass, int symbol,
                                           const char *field_name) {
    return xr_class_transition_get_or_create_impl(X, klass, symbol, field_name, false);
}

static XrClass *xr_class_build_dynamic_chain(XrVMRuntime *X, XrClass *root,
                                             const char *const *names, int count, bool sealed,
                                             const char *label) {
    XR_DCHECK(X != NULL, "build_dynamic_chain: NULL isolate");
    XR_DCHECK(root != NULL, "build_dynamic_chain: NULL root");

    XrClass *cur = root;
    if (count > 0 && names != NULL) {
        XrSymbolTable *st = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
        XR_DCHECK(st != NULL, "build_dynamic_chain: NULL symbol table");
        for (int i = 0; i < count; i++) {
            int sym = (int) xr_symbol_register_in_table(st, names[i]);
            const char *interned = xr_symbol_get_name_in_table(st, sym);
            XrClass *next = xr_class_transition_get_or_create_impl(
                X, cur, sym, interned ? interned : names[i], true);
            if (!next)
                return NULL;
            cur = next;
        }
    }
    if (sealed) {
        cur->flags |= XR_CLASS_DYNAMIC_SEALED;
    }
    (void) label;
    return cur;
}

XrClass *xr_class_build_json_chain(XrVMRuntime *X, const char *const *names, int count,
                                   bool sealed) {
    XR_DCHECK(X != NULL, "build_json_chain: NULL isolate");
    XR_DCHECK(X->core != NULL && X->core->jsonRootClass != NULL,
              "build_json_chain: jsonRootClass not initialized");
    return xr_class_build_dynamic_chain(X, X->core->jsonRootClass, names, count, sealed, "Json");
}

XrClass *xr_class_build_record_chain(XrVMRuntime *X, const char *const *names, int count,
                                     bool sealed) {
    XR_DCHECK(X != NULL, "build_record_chain: NULL isolate");
    XR_DCHECK(X->core != NULL && X->core->recordRootClass != NULL,
              "build_record_chain: recordRootClass not initialized");
    XrClass *root = sealed ? X->core->recordSealedRootClass : X->core->recordRootClass;
    XR_DCHECK(root != NULL, "build_record_chain: selected root not initialized");
    return xr_class_build_dynamic_chain(X, root, names, count, sealed, "Record");
}
