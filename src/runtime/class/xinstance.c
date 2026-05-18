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
#include "xclass.h"
#include "xmethod.h"
#include "../../base/xmalloc.h"
#include "../gc/xgc.h"
#include "../gc/xalloc_unified.h"
#include "../object/xstring.h"
#include "../symbol/xsymbol_table.h"
#include "../xisolate_api.h"
#include "../xvm_call.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ========== Instance Operations ========== */

XrInstance *xr_instance_new(XrayIsolate *X, XrClass *cls) {
    XR_DCHECK(cls != NULL, "Class must not be NULL");

    uint32_t field_count = xr_class_instance_field_count(cls);
    const char *class_name = cls->name ? cls->name : "<unnamed>";

    size_t size = xr_instance_size(cls);
    // Instances are regular GC objects on the running coroutine's heap:
    // xcoro_gc_traverse handles XR_TINSTANCE field walking and the type
    // is in HAS_REFS_BITMAP. Falling back to isolate fixedgc keeps the
    // bootstrap path working before main_coro is ready (and matches
    // deep-copy's fallback when dst_coro_gc is unavailable).
    XrInstance *inst = NULL;
    XrCoroutine *coro = xr_current_coro(X);
    if (coro) {
        inst = (XrInstance *) xr_alloc(coro, size, XR_TINSTANCE);
    } else {
        inst = (XrInstance *) xr_gc_alloc(xr_isolate_get_gc(X), size, XR_TINSTANCE);
    }

    if (!inst) {
        xr_log_warning("instance", "failed to allocate instance of class %s", class_name);
        return NULL;
    }

    xr_gc_header_init_type(&inst->gc, XR_TINSTANCE);
    inst->klass = cls;

    if (cls->field_default_values) {
        for (uint32_t i = 0; i < field_count; i++) {
            inst->fields[i] = cls->field_default_values[i];
        }
    } else {
        for (uint32_t i = 0; i < field_count; i++) {
            inst->fields[i] = xr_null();
        }
    }

    // Initialize native body if present
    XrNativeBodyDesc *desc = cls->native_body;
    if (desc && desc->init) {
        void *body = xr_instance_native_body(inst);
        XR_DCHECK(body != NULL, "native body pointer must not be NULL");
        desc->init(inst, body);
    }

    return inst;
}

void xr_instance_init_inplace(XrInstance *inst, XrClass *cls) {
    if (!inst || !cls)
        return;

    inst->klass = cls;

    // Dynamic-layout: zero all reserved in-object slots (capacity, not just
    // current field_count) so subsequent transitions can write in-bounds.
    uint32_t slot_count;
    if (cls->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        slot_count = cls->in_object_capacity;
    } else {
        slot_count = xr_class_instance_field_count(cls);
    }

    if (cls->field_default_values && !(cls->flags & XR_CLASS_DYNAMIC_LAYOUT)) {
        for (uint32_t i = 0; i < slot_count; i++) {
            inst->fields[i] = cls->field_default_values[i];
        }
    } else {
        for (uint32_t i = 0; i < slot_count; i++) {
            inst->fields[i] = xr_null();
        }
    }
}

size_t xr_instance_size(XrClass *cls) {
    XR_DCHECK(cls != NULL, "instance_size: NULL cls");
    // Dynamic-layout instances reserve in_object_capacity slots so transitions
    // that grow field_count don't require reallocating the instance. The last
    // slot doubles as the overflow pointer when field_count > capacity - 1.
    uint32_t slot_count;
    if (cls->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        slot_count = cls->in_object_capacity;
    } else {
        slot_count = xr_class_instance_field_count(cls);
    }
    size_t size = sizeof(XrInstance) + sizeof(XrValue) * slot_count;
    XrNativeBodyDesc *desc = cls->native_body;
    if (desc) {
        size_t align = desc->body_align ? (size_t) desc->body_align : sizeof(void *);
        size = (size + align - 1) & ~(align - 1);
        size += desc->body_size;
    }
    return size;
}

void xr_instance_free(XrInstance *inst) {
    (void) inst;
    // Fields managed by GC, flexible array released with object
}

void xr_gc_destroy_instance(XrGCHeader *obj, struct XrCoroGC *owning_gc) {
    (void) owning_gc;
    if (!obj)
        return;
    XrInstance *inst = (XrInstance *) obj;
    XrClass *klass = inst->klass;
    if (!klass)
        return;

    // Free overflow buffer for dynamic-layout instances
    if (klass->flags & XR_CLASS_DYNAMIC_LAYOUT) {
        uint16_t cap = klass->in_object_capacity;
        if (cap > 0 && xr_class_instance_field_count(klass) > cap - 1) {
            XrValue *overflow = (XrValue *) inst->fields[cap - 1].ptr;
            if (overflow) {
                int64_t *raw = ((int64_t *) overflow) - 1;
                xr_free(raw);
            }
        }
    }

    // Call native body destructor
    XrNativeBodyDesc *desc = klass->native_body;
    if (desc && desc->destroy) {
        void *body = xr_instance_native_body(inst);
        XR_DCHECK(body != NULL, "destroy: native body NULL but desc present");
        desc->destroy(body);
    }
}

XrInstance *xr_instance_clone(XrayIsolate *X, XrInstance *src) {
    XR_DCHECK(src != NULL, "instance_clone: NULL src");
    XrClass *cls = src->klass;
    XR_DCHECK(cls != NULL, "instance_clone: NULL klass");

    uint32_t field_count = xr_class_instance_field_count(cls);
    size_t size = xr_instance_size(cls);
    // Same owner choice as xr_instance_new: clone lands on the running
    // coroutine's heap, with isolate fixedgc as bootstrap fallback.
    XrInstance *dst = NULL;
    XrCoroutine *coro = xr_current_coro(X);
    if (coro) {
        dst = (XrInstance *) xr_alloc(coro, size, XR_TINSTANCE);
    } else {
        dst = (XrInstance *) xr_gc_alloc(xr_isolate_get_gc(X), size, XR_TINSTANCE);
    }
    if (!dst)
        return NULL;

    xr_gc_header_init_type(&dst->gc, XR_TINSTANCE);
    dst->klass = cls;
    memcpy(dst->fields, src->fields, sizeof(XrValue) * field_count);

    // Shallow-copy native body bytes (caller is responsible for deep semantics)
    XrNativeBodyDesc *desc = cls->native_body;
    if (desc) {
        void *src_body = xr_instance_native_body(src);
        void *dst_body = xr_instance_native_body(dst);
        memcpy(dst_body, src_body, desc->body_size);
    }
    return dst;
}

// Access control handled by compiler/interpreter
XrValue xr_instance_get_field(XrayIsolate *X, XrInstance *inst, const char *name) {
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

void xr_instance_set_field(XrayIsolate *X, XrInstance *inst, const char *name, XrValue value) {
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

    inst->fields[index] = value;
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), inst);
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
    inst->fields[index] = value;
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), inst);
}

XrValue xr_instance_call_method(XrayIsolate *X, XrInstance *inst, const char *name, XrValue *args,
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
            printf("%g", XR_TO_FLOAT(val));
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

/* ========== Dynamic Layout Field Access ========== */

XrValue xr_instance_get_dynamic_field(XrInstance *inst, uint16_t index) {
    XR_DCHECK(inst != NULL, "get_dynamic_field: NULL inst");
    XrClass *klass = inst->klass;
    XR_DCHECK(klass->flags & XR_CLASS_DYNAMIC_LAYOUT, "get_dynamic_field: not dynamic");
    uint16_t cap = klass->in_object_capacity;
    XR_DCHECK(cap > 0, "get_dynamic_field: zero capacity");

    if (index < cap - 1) {
        return inst->fields[index];
    }
    // Overflow: fields[cap-1] holds raw pointer to heap array
    XrValue *overflow = (XrValue *) inst->fields[cap - 1].ptr;
    if (!overflow)
        return xr_null();
    uint16_t overflow_idx = index - (cap - 1);
    return overflow[overflow_idx];
}

bool xr_instance_set_dynamic_field(XrayIsolate *X, XrInstance *inst, uint16_t index,
                                   XrValue value) {
    XR_DCHECK(inst != NULL, "set_dynamic_field: NULL inst");
    XR_DCHECK(X != NULL, "set_dynamic_field: NULL isolate");
    XrClass *klass = inst->klass;
    XR_DCHECK(klass->flags & XR_CLASS_DYNAMIC_LAYOUT, "set_dynamic_field: not dynamic");
    uint16_t cap = klass->in_object_capacity;
    XR_DCHECK(cap > 0, "set_dynamic_field: zero capacity");

    if (index < cap - 1) {
        inst->fields[index] = value;
        return true;
    }

    // Overflow region
    uint16_t overflow_idx = index - (cap - 1);
    XrValue *overflow = (XrValue *) inst->fields[cap - 1].ptr;

    // Determine current overflow capacity (stored as length in slot if exists)
    uint16_t overflow_cap = 0;
    if (overflow) {
        // Capacity tag stored at overflow[-1] as raw int
        overflow_cap = (uint16_t) ((int64_t *) overflow)[-1];
    }

    if (overflow_idx >= overflow_cap) {
        // Grow: double or start at 8
        uint16_t new_cap = overflow_cap == 0 ? 8 : overflow_cap * 2;
        while (new_cap <= overflow_idx)
            new_cap *= 2;
        // Layout: [capacity_tag][values...]
        size_t alloc_size = sizeof(int64_t) + sizeof(XrValue) * new_cap;
        int64_t *raw = (int64_t *) xr_malloc(alloc_size);
        if (!raw)
            return false;
        raw[0] = (int64_t) new_cap;
        XrValue *new_overflow = (XrValue *) (raw + 1);
        // Copy old data
        for (uint16_t i = 0; i < overflow_cap; i++)
            new_overflow[i] = overflow[i];
        // Zero-init new slots
        for (uint16_t i = overflow_cap; i < new_cap; i++)
            new_overflow[i] = xr_null();
        // Free old
        if (overflow) {
            int64_t *old_raw = ((int64_t *) overflow) - 1;
            xr_free(old_raw);
        }
        overflow = new_overflow;
        inst->fields[cap - 1].ptr = (void *) overflow;
    }

    overflow[overflow_idx] = value;
    return true;
}

/* ========== Class Transition ========== */

XrClass *xr_class_transition_get_or_create(XrayIsolate *X, XrClass *klass, int symbol,
                                           const char *field_name) {
    XR_DCHECK(X != NULL, "transition: NULL isolate");
    XR_DCHECK(klass != NULL, "transition: NULL klass");
    XR_DCHECK(klass->flags & XR_CLASS_DYNAMIC_LAYOUT, "transition: not dynamic");

    // Search existing transitions
    for (XrClassTransition *t = klass->transitions; t; t = t->next) {
        if (t->symbol == symbol)
            return t->target;
    }

    // Sealed dynamic class rejects new field additions
    if (klass->flags & XR_CLASS_DYNAMIC_SEALED) {
        return NULL;
    }

    // Create child class: inherits all parent fields + one new field
    uint16_t parent_fc = klass->field_count;
    uint16_t child_fc = parent_fc + 1;

    XrClass *child = (XrClass *) xr_calloc(1, sizeof(XrClass));
    if (!child)
        return NULL;

    // Copy key fields from parent
    child->name = klass->name;
    child->super = klass->super;
    child->flags = klass->flags;
    child->in_object_capacity = klass->in_object_capacity;
    child->transition_parent = klass;
    child->transition_symbol = symbol;
    child->transitions = NULL;

    // Build field descriptor array: parent fields + new field
    child->fields = (XrFieldDescriptor *) xr_malloc(sizeof(XrFieldDescriptor) * child_fc);
    if (!child->fields) {
        xr_free(child);
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

    // Register transition on parent
    XrClassTransition *trans = (XrClassTransition *) xr_malloc(sizeof(XrClassTransition));
    if (!trans) {
        xr_free(child->field_symbol_to_index);
        xr_free(child->fields);
        xr_free(child);
        return NULL;
    }
    trans->symbol = symbol;
    trans->target = child;
    trans->next = klass->transitions;
    klass->transitions = trans;

    return child;
}

XrClass *xr_class_build_json_chain(XrayIsolate *X, const char *const *names, int count,
                                   bool sealed) {
    XR_DCHECK(X != NULL, "build_json_chain: NULL isolate");
    XR_DCHECK(X->core != NULL && X->core->jsonRootClass != NULL,
              "build_json_chain: jsonRootClass not initialized");

    XrClass *cur = X->core->jsonRootClass;
    if (count > 0 && names != NULL) {
        XrSymbolTable *st = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
        XR_DCHECK(st != NULL, "build_json_chain: NULL symbol table");
        for (int i = 0; i < count; i++) {
            int sym = (int) xr_symbol_register_in_table(st, names[i]);
            const char *interned = xr_symbol_get_name_in_table(st, sym);
            XrClass *next =
                xr_class_transition_get_or_create(X, cur, sym, interned ? interned : names[i]);
            if (!next)
                return NULL;
            cur = next;
        }
    }
    if (sealed) {
        cur->flags |= XR_CLASS_DYNAMIC_SEALED;
    }
    return cur;
}
