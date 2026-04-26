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

XrInstance* xr_instance_new(XrayIsolate *X, XrClass *cls) {
    XR_DCHECK(cls != NULL, "Class must not be NULL");

    uint32_t field_count = xr_class_instance_field_count(cls);
    const char* class_name = cls->name ? cls->name : "<unnamed>";

    size_t size = sizeof(XrInstance) + sizeof(XrValue) * field_count;
    // Instances are regular GC objects on the running coroutine's heap:
    // xcoro_gc_traverse handles XR_TINSTANCE field walking and the type
    // is in HAS_REFS_BITMAP. Falling back to isolate fixedgc keeps the
    // bootstrap path working before main_coro is ready (and matches
    // deep-copy's fallback when dst_coro_gc is unavailable).
    XrInstance *inst = NULL;
    XrCoroutine *coro = xr_current_coro(X);
    if (coro) {
        inst = (XrInstance*)xr_alloc(coro, size, XR_TINSTANCE);
    } else {
        inst = (XrInstance*)xr_gc_alloc(xr_isolate_get_gc(X), size, XR_TINSTANCE);
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

    return inst;
}

void xr_instance_init_inplace(XrInstance *inst, XrClass *cls) {
    if (!inst || !cls) return;

    inst->klass = cls;

    uint32_t field_count = xr_class_instance_field_count(cls);

    if (cls->field_default_values) {
        for (uint32_t i = 0; i < field_count; i++) {
            inst->fields[i] = cls->field_default_values[i];
        }
    } else {
        for (uint32_t i = 0; i < field_count; i++) {
            inst->fields[i] = xr_null();
        }
    }
}

size_t xr_instance_size(XrClass *cls) {
    XR_DCHECK(cls != NULL, "instance_size: NULL cls");
    uint32_t field_count = xr_class_instance_field_count(cls);
    return sizeof(XrInstance) + sizeof(XrValue) * field_count;
}

void xr_instance_free(XrInstance *inst) {
    (void)inst;
    // Fields managed by GC, flexible array released with object
}

XrInstance* xr_instance_clone(XrayIsolate *X, XrInstance *src) {
    XR_DCHECK(src != NULL, "instance_clone: NULL src");
    XrClass *cls = src->klass;
    XR_DCHECK(cls != NULL, "instance_clone: NULL klass");

    uint32_t field_count = xr_class_instance_field_count(cls);
    size_t size = sizeof(XrInstance) + sizeof(XrValue) * field_count;
    // Same owner choice as xr_instance_new: clone lands on the running
    // coroutine's heap, with isolate fixedgc as bootstrap fallback.
    XrInstance *dst = NULL;
    XrCoroutine *coro = xr_current_coro(X);
    if (coro) {
        dst = (XrInstance*)xr_alloc(coro, size, XR_TINSTANCE);
    } else {
        dst = (XrInstance*)xr_gc_alloc(xr_isolate_get_gc(X), size, XR_TINSTANCE);
    }
    if (!dst) return NULL;

    xr_gc_header_init_type(&dst->gc, XR_TINSTANCE);
    dst->klass = cls;
    memcpy(dst->fields, src->fields, sizeof(XrValue) * field_count);
    return dst;
}

// Access control handled by compiler/interpreter
XrValue xr_instance_get_field(XrayIsolate *X, XrInstance *inst, const char *name) {
    if (!X || !inst || !name) return xr_null();

    XrClass *klass = xr_instance_get_class(inst);
    if (!klass) return xr_null();

    int index = xr_class_lookup_field_by_name(X, klass, name);
    if (index < 0) {
        xr_log_warning("instance", "field '%s' not found in class '%s'",
                       name, klass->name ? klass->name : "<unnamed>");
        return xr_null();
    }

    // Bounds check: ensure index is within instance field range
    int ifc = xr_class_instance_field_count(klass);
    if (index >= ifc) {
        xr_log_warning("instance", "field index %d out of bounds (max %d)",
                       index, ifc - 1);
        return xr_null();
    }

    return inst->fields[index];
}

void xr_instance_set_field(XrayIsolate *X, XrInstance *inst, const char *name, XrValue value) {
    if (!X || !inst || !name) return;

    XrClass *klass = xr_instance_get_class(inst);
    if (!klass) return;

    int index = xr_class_lookup_field_by_name(X, klass, name);
    if (index < 0) {
        xr_log_warning("instance", "field '%s' not found in class '%s'",
                       name, klass->name ? klass->name : "<unnamed>");
        return;
    }

    // Bounds check: ensure index is within instance field range
    int ifc = xr_class_instance_field_count(klass);
    if (index >= ifc) {
        xr_log_warning("instance", "field index %d out of bounds (max %d)",
                       index, ifc - 1);
        return;
    }

    inst->fields[index] = value;
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), inst);
}

XrValue xr_instance_get_field_by_index(XrInstance *inst, int index) {
    XR_DCHECK(inst != NULL, "Instance must not be NULL");
    XrClass *klass = xr_instance_get_class(inst);
    XR_DCHECK_BOUNDS(index, klass->field_count, "field index out of bounds");
    (void)klass;
    return inst->fields[index];
}

void xr_instance_set_field_by_index(XrInstance *inst, int index, XrValue value) {
    XR_DCHECK(inst != NULL, "Instance must not be NULL");
    XrClass *klass = xr_instance_get_class(inst);
    XR_DCHECK_BOUNDS(index, klass->field_count, "field index out of bounds");
    (void)klass;
    inst->fields[index] = value;
    XR_GC_BARRIER_BACK_SAFE(xr_current_coro_gc(), inst);
}

XrValue xr_instance_call_method(XrayIsolate *X, XrInstance *inst,
                                 const char *name,
                                XrValue *args, int argc) {
    if (!X || !inst || !name) return xr_null();

    XrClass *klass = xr_instance_get_class(inst);
    if (!klass) return xr_null();

    // Convert method name to symbol
    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(X);
    if (!sym_table) {
        xr_log_warning("instance", "symbol table not available");
        return xr_null();
    }

    SymbolId method_symbol = xr_symbol_lookup_in_table(sym_table, name);
    if (method_symbol == SYMBOL_INVALID) {
        xr_log_warning("instance", "method '%s' not found in class '%s'",
                       name, klass->name ? klass->name : "<unnamed>");
        return xr_null();
    }

    XrMethod *method = xr_class_lookup_method(klass, method_symbol);
    if (!method) {
        xr_log_warning("instance", "method '%s' not found in class '%s'",
                       name, klass->name ? klass->name : "<unnamed>");
        return xr_null();
    }

    XrValue this_value = xr_value_from_instance(inst);

    // Stack buffer for small arg counts, heap fallback for large
    XrValue stack_buf[9];
    XrValue *full_args = (argc + 1 <= 9) ? stack_buf
        : (XrValue*)xr_malloc(sizeof(XrValue) * (argc + 1));
    if (!full_args) {
        xr_log_warning("instance", "failed to allocate argument array");
        return xr_null();
    }
    full_args[0] = this_value;
    for (int i = 0; i < argc; i++) {
        full_args[i + 1] = args[i];
    }

    // Call method based on type
    XrValue result = xr_null();
    if (method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
        result = method->as.primitive(X, full_args, argc + 1);
    } else if (method->as.closure) {
        result = xr_vm_call_closure(X, method->as.closure, full_args, argc + 1);
    }

    if (full_args != stack_buf) xr_free(full_args);
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

    const char *class_name = klass->name ? klass->name : "<unnamed>";
    printf("%s instance {\n", class_name);

    // Use instance field count (exclude static fields)
    int ifc = xr_class_instance_field_count(klass);

    for (int i = 0; i < ifc; i++) {
        const char *field_name = (klass->fields && i < klass->field_count && klass->fields[i].name)
            ? klass->fields[i].name : "unknown";
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
    if (!inst || !cls) return false;
    return xr_class_instanceof(xr_instance_get_class(inst), cls);
}
