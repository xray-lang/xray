/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_class.h - AOT class runtime: type table, object allocation
 *
 * KEY CONCEPT:
 *   All heap objects (class instances, promoted structs) share XrtArcHdr
 *   from xrt_arc.h as their common header.  XrtArcHdr.type indexes into
 *   xrt_type_table[] for class metadata (name, parent, vtable, destructor).
 *
 *   Field access is via C struct members (compile-time offsets).
 *   Method dispatch:
 *     - Known type -> direct C call (most cases)
 *     - Polymorphic -> vtable[slot_index] indirect call
 *     - instanceof -> walk parent chain in type table
 *
 * GENERATED CODE PATTERN:
 *   // --- module init ---
 *   static uint16_t _tid_Point;
 *   _tid_Point = xrt_type_register("Point", 0, NULL, 0, NULL, nfields*16);
 *
 *   // --- constructor call ---
 *   { XrValue _inst = xrt_mkptr(xrt_obj_alloc(_tid_Point, nfields*16), XRT_TAG_PTR);
 *     xr_constructor(xrt_ctx, _inst, ...);
 *     v5 = _inst; }
 *
 * RELATED MODULES:
 *   - xrt_arc.h: XrtArcHdr, ARC retain/release, bump allocator
 *   - xrt_value.h: XrtValue tagged union (PTR tag carries object pointer)
 *   - xcgen.c: emits class type registration and constructor calls
 */

#ifndef XRT_CLASS_H
#define XRT_CLASS_H

#include "xrt_value.h"
#include "xrt_arc.h"  // XrtArcHdr, XRT_ARC_HDR, xrt_arc_alloc, macros
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Type Info — one entry per class/struct type in the global type table.
 * XrtArcHdr.type is the index into xrt_type_table[].
 * ========================================================================= */

typedef void (*XrtDestructor)(void *obj);
typedef XrtValue (*XrtMethodFn)(void);  // generic fn ptr placeholder

typedef struct {
    uint16_t type_id;
    uint16_t parent_id;   // 0 = no parent
    const char *name;     // class name (e.g. "Point")
    XrtMethodFn *vtable;  // virtual method table (NULL if no virtuals)
    int vtable_size;
    XrtDestructor destructor;  // NULL for classes without custom dtor
    uint32_t instance_size;    // byte size of instance fields
} XrtTypeInfo;

/* =========================================================================
 * Type table — populated by generated code at module init
 *
 * Index 0 is reserved (no type / null).
 * Max 256 types in a single AOT binary for now.
 * ========================================================================= */

#define XRT_MAX_TYPES 256

#ifdef XRT_IMPL
XrtTypeInfo xrt_type_table[XRT_MAX_TYPES];
uint16_t xrt_type_count = 1;  // 0 reserved
#else
extern XrtTypeInfo xrt_type_table[];
extern uint16_t xrt_type_count;
#endif

/* Register a type; returns assigned type_id */
static inline uint16_t xrt_type_register(const char *name, uint16_t parent_id, XrtMethodFn *vtable,
                                         int vtable_size, XrtDestructor dtor, uint32_t inst_size) {
    if (xrt_type_count >= XRT_MAX_TYPES) {
        fprintf(stderr, "xrt_type_register: type table full\n");
        abort();
    }
    uint16_t id = xrt_type_count++;
    XrtTypeInfo *ti = &xrt_type_table[id];
    ti->type_id = id;
    ti->parent_id = parent_id;
    ti->name = name;
    ti->vtable = vtable;
    ti->vtable_size = vtable_size;
    ti->destructor = dtor;
    ti->instance_size = inst_size;
    return id;
}

/* =========================================================================
 * Object allocation — ARC alloc + set type in XrtArcHdr
 *
 * Uses xrt_arc_alloc (supports bump allocator) and stores the type_id
 * in XrtArcHdr.type for vtable dispatch and instanceof.
 * ========================================================================= */

static inline void *xrt_obj_alloc(uint16_t type_id, uint32_t size) {
    void *obj = xrt_arc_alloc((size_t) size);
    XrtArcHdr *h = XRT_ARC_HDR(obj);
    h->type = type_id;
    h->flags |= XRT_ARC_HAS_DEINIT;  // enable destructor dispatch on release
    return obj;
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

#endif  // XRT_CLASS_H
