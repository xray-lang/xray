/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_class.h - AOT class runtime: object header, type table, vtable dispatch
 *
 * KEY CONCEPT:
 *   Heap-allocated class instances have an XrtObjHeader prefix containing
 *   type_id and ARC refcount.  Each class has an XrtTypeInfo entry in a
 *   global type table that records the parent type, vtable, and destructor.
 *
 *   Field access is via C struct members (compile-time offsets).
 *   Method dispatch:
 *     - Known type → direct C call (most cases)
 *     - Polymorphic → vtable[slot_index] indirect call
 *     - instanceof → walk parent chain in type table
 *
 * GENERATED CODE PATTERN:
 *   // --- per-class struct ---
 *   typedef struct {
 *       XrtObjHeader hdr;   // type_id + refcount
 *       int64_t x;
 *       int64_t y;
 *   } XrtObj_Point;
 *
 *   // --- constructor ---
 *   static XrtObj_Point *xr_Point_new(XrtContext ctx, int64_t x, int64_t y) {
 *       XrtObj_Point *self = (XrtObj_Point *)xrt_obj_alloc(TYPE_ID_POINT, sizeof(XrtObj_Point));
 *       self->x = x;
 *       self->y = y;
 *       return self;
 *   }
 *
 * RELATED MODULES:
 *   - xrt_arc.h: ARC retain/release (operates on XrtObjHeader)
 *   - xrt_value.h: XrtValue tagged union (PTR tag carries object pointer)
 *   - xcgen.c: emits class structs and type table from class descriptors
 */

#ifndef XRT_CLASS_H
#define XRT_CLASS_H

#include "xrt_value.h"
#include "xrt_arc.h" // XRT_CALLOC / XRT_FREE macros
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Object Header — prefix of every heap-allocated class instance
 *
 * Placed at offset 0 so (XrtObjHeader*)obj == obj.  Field access
 * casts to the concrete struct type: ((XrtObj_Foo*)obj)->field.
 * ========================================================================= */

typedef struct {
    uint16_t type_id;     // index into xrt_type_table[]
    uint16_t _pad;        // alignment padding
    uint32_t refcount;    // ARC reference count (0 = immortal/stack)
} XrtObjHeader;

/* =========================================================================
 * Type Info — one entry per class in the global type table
 * ========================================================================= */

typedef void (*XrtDestructor)(void *obj);
typedef XrtValue (*XrtMethodFn)(void);  // generic fn ptr placeholder

typedef struct {
    uint16_t       type_id;
    uint16_t       parent_id;     // 0 = no parent
    const char    *name;          // class name (e.g. "Point")
    XrtMethodFn   *vtable;        // virtual method table (NULL if no virtuals)
    int            vtable_size;
    XrtDestructor  destructor;    // NULL for classes without custom dtor
    uint32_t       instance_size; // sizeof(XrtObj_Foo)
} XrtTypeInfo;

/* =========================================================================
 * Type table — populated by generated code at module init
 *
 * Index 0 is reserved (no type / null).
 * Max 256 types in a single AOT binary for now.
 * ========================================================================= */

#define XRT_MAX_TYPES 256

static XrtTypeInfo xrt_type_table[XRT_MAX_TYPES];
static uint16_t    xrt_type_count = 1;  // 0 reserved

/* Register a type; returns assigned type_id */
static inline uint16_t xrt_type_register(const char *name, uint16_t parent_id,
                                          XrtMethodFn *vtable, int vtable_size,
                                          XrtDestructor dtor, uint32_t inst_size) {
    if (xrt_type_count >= XRT_MAX_TYPES) {
        fprintf(stderr, "xrt_type_register: type table full\n");
        abort();
    }
    uint16_t id = xrt_type_count++;
    XrtTypeInfo *ti = &xrt_type_table[id];
    ti->type_id      = id;
    ti->parent_id    = parent_id;
    ti->name         = name;
    ti->vtable       = vtable;
    ti->vtable_size  = vtable_size;
    ti->destructor   = dtor;
    ti->instance_size = inst_size;
    return id;
}

/* =========================================================================
 * Object allocation — alloc + init header
 * ========================================================================= */

static inline void *xrt_obj_alloc(uint16_t type_id, uint32_t size) {
    void *obj = XRT_CALLOC(1, size);
    if (!obj) {
        fprintf(stderr, "xrt_obj_alloc: out of memory\n");
        abort();
    }
    XrtObjHeader *hdr = (XrtObjHeader *)obj;
    hdr->type_id  = type_id;
    hdr->refcount = 1;  // born with refcount 1
    return obj;
}

/* =========================================================================
 * ARC operations — retain / release operating on XrtObjHeader
 * ========================================================================= */

static inline void xrt_obj_retain(void *obj) {
    if (!obj) return;
    XrtObjHeader *h = (XrtObjHeader *)obj;
    if (h->refcount > 0) h->refcount++;  // 0 = immortal
}

static inline void xrt_obj_release(void *obj) {
    if (!obj) return;
    XrtObjHeader *h = (XrtObjHeader *)obj;
    if (h->refcount == 0) return;  // immortal
    if (--h->refcount == 0) {
        XrtTypeInfo *ti = &xrt_type_table[h->type_id];
        if (ti->destructor) ti->destructor(obj);
        XRT_FREE(obj);
    }
}

/* For XrtValue (tagged) — retain/release when tag == PTR */
static inline void xrt_val_retain(XrtValue v) {
    if (v.tag == XRT_TAG_PTR && v.ptr) xrt_obj_retain(v.ptr);
}

static inline void xrt_val_release(XrtValue v) {
    if (v.tag == XRT_TAG_PTR && v.ptr) xrt_obj_release(v.ptr);
}

/* =========================================================================
 * instanceof — walk parent chain
 * ========================================================================= */

static inline bool xrt_instanceof(void *obj, uint16_t target_type_id) {
    if (!obj) return false;
    XrtObjHeader *h = (XrtObjHeader *)obj;
    uint16_t tid = h->type_id;
    while (tid != 0) {
        if (tid == target_type_id) return true;
        tid = xrt_type_table[tid].parent_id;
    }
    return false;
}

/* =========================================================================
 * vtable dispatch helper
 *
 * Usage:  result = XRT_VCALL(obj, slot_index, RetType, (ArgTypes...))(args);
 *
 * Inline helper for single-dispatch:
 *   xrt_vcall(obj, slot) returns the raw function pointer.
 * ========================================================================= */

static inline XrtMethodFn xrt_vcall(void *obj, int slot) {
    XrtObjHeader *h = (XrtObjHeader *)obj;
    XrtTypeInfo *ti = &xrt_type_table[h->type_id];
    return ti->vtable[slot];
}

/* Box an object pointer into XrtValue */
static inline XrtValue xrt_box_obj(void *obj) {
    XrtValue v;
    v.ptr = obj;
    v.tag = obj ? XRT_TAG_PTR : XRT_TAG_NULL;
    return v;
}

/* Unbox XrtValue to object pointer (no type check) */
static inline void *xrt_unbox_obj(XrtValue v) {
    return v.ptr;
}

#endif // XRT_CLASS_H
