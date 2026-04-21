/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xinstance.h - Instance objects for class system
 *
 * KEY CONCEPT:
 *   Flexible array member stores field values.
 *   Field order: superclass fields first, then this class.
 *   Compact GC header for memory efficiency.
 *
 * MEMORY LAYOUT:
 *
 *   XrInstance (variable size)
 *   +------------------+
 *   | XrGCHeader gc    |  8 bytes (type tag + flags + classObj)
 *   +------------------+
 *   | klass            |  8 bytes (-> XrClass)
 *   +------------------+
 *   | fields[0]        |  16 bytes (inherited from grandparent)
 *   | fields[1]        |  16 bytes (inherited from parent)
 *   | ...              |
 *   | fields[n-1]      |  16 bytes (this class's last field)
 *   +------------------+
 *
 * FIELD INHERITANCE:
 *
 *   class Animal { let name }           // field_count=1, fields=[name]
 *   class Dog extends Animal { let age } // field_count=2, fields=[name, age]
 *   class Husky extends Dog { let color } // field_count=3, fields=[name, age, color]
 *
 *   Husky instance memory:
 *   +------------------+
 *   | gc               |
 *   | klass -> Husky   |
 *   +------------------+
 *   | fields[0]: name  |  <- Animal's field (index 0)
 *   | fields[1]: age   |  <- Dog's field (index 1)
 *   | fields[2]: color |  <- Husky's field (index 2)
 *   +------------------+
 *
 * WHY THIS DESIGN:
 *   - Single contiguous allocation (cache friendly)
 *   - O(1) field access by index (no indirection)
 *   - Parent fields at fixed offsets (no vtable lookup)
 *   - Flexible array avoids extra pointer + allocation
 */

#ifndef XINSTANCE_H
#define XINSTANCE_H

#include "../value/xvalue.h"
#include "xclass.h"
#include "../gc/xgc_header.h"

struct XrInstance {
    XrGCHeader gc;
    struct XrClass *klass;
    XrValue fields[];          // Flexible array member
};

/* ========== Instance Operations ========== */

// All fields initialized to null, constructor not called
XR_FUNC XrInstance* xr_instance_new(XrayIsolate *X, XrClass *cls);

XR_FUNC void xr_instance_init_inplace(XrInstance *inst, XrClass *cls);
XR_FUNC size_t xr_instance_size(XrClass *cls);
XR_FUNC void xr_instance_free(XrInstance *inst);

XR_FUNC XrValue xr_instance_get_field(XrayIsolate *X, XrInstance *inst, const char *name);
XR_FUNC void xr_instance_set_field(XrayIsolate *X, XrInstance *inst, const char *name, XrValue value);

// Fast path by index
XR_FUNC XrValue xr_instance_get_field_by_index(XrInstance *inst, int index);
XR_FUNC void xr_instance_set_field_by_index(XrInstance *inst, int index, XrValue value);

XR_FUNC XrValue xr_instance_call_method(XrayIsolate *X, XrInstance *inst,
                                 const char *name,
                                 XrValue *args, int argc);

// Allocate + init fields + call constructor
XR_FUNC XrValue xr_instance_construct(XrayIsolate *X, XrClass *cls,
                               XrValue *args, int argc);

// Shallow clone: allocate new instance, copy all field values
XR_FUNC XrInstance* xr_instance_clone(XrayIsolate *X, XrInstance *src);

/* ========== Debug ========== */

XR_FUNC void xr_instance_print(XrInstance *inst);
XR_FUNC bool xr_instance_is_a(XrInstance *inst, XrClass *cls);

/* ========== Inline Accessors ========== */

static inline XrClass* xr_instance_get_class(XrInstance *inst) {
    return inst->klass;
}

static inline XrValue xr_instance_get_field_fast(XrInstance *inst, int index) {
    return inst->fields[index];
}

static inline void xr_instance_set_field_fast(XrInstance *inst, int index, XrValue value) {
    inst->fields[index] = value;
}

#endif // XINSTANCE_H
