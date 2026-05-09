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
 *   All heap objects (class instances, promoted structs) carry XrtArcHdr
 *   from xrt_arc.h as a common header.  XrtArcHdr.type indexes into
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
 *   { XrValue _inst = xr_mkptr(xrt_obj_alloc(_tid_Point, nfields*16), XR_TAG_PTR);
 *     xr_constructor(xrt_ctx, _inst, ...);
 *     v5 = _inst; }
 *
 * RELATED MODULES:
 *   - xrt_arc.h: XrtArcHdr, bump allocator
 *   - xrt_value.h: XrValue tagged union (PTR tag carries object pointer)
 *   - xi_cgen.c: emits class type registration and constructor calls
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
typedef XrValue (*XrtMethodFn)(void);  // generic fn ptr placeholder

typedef struct {
    uint16_t type_id;
    uint16_t parent_id;                // 0 = no parent
    const char *name;                  // internal name (e.g. "Box$i64")
    const char *display_name;          // user-visible name (e.g. "Box"); NULL = same as name
    uint16_t generic_origin;           // type_id of skeleton class; 0 = not monomorphized
    const char **mono_type_arg_names;  // static array of display name strings, NULL if not generic
    uint8_t mono_type_argc;            // element count of mono_type_arg_names
    XrtMethodFn *vtable;               // virtual method table (NULL if no virtuals)
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
    ti->display_name = NULL;
    ti->generic_origin = 0;
    ti->mono_type_arg_names = NULL;
    ti->mono_type_argc = 0;
    ti->vtable = vtable;
    ti->vtable_size = vtable_size;
    ti->destructor = dtor;
    ti->instance_size = inst_size;
    return id;
}

/* Set generic origin and display name for a monomorphized type.
 * type_arg_names is a static array of string literals (no ownership transfer). */
static inline void xrt_type_set_generic(uint16_t type_id, uint16_t origin_id, const char *display,
                                        const char **type_arg_names, uint8_t argc) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return;
    XrtTypeInfo *ti = &xrt_type_table[type_id];
    ti->generic_origin = origin_id;
    ti->display_name = display;
    ti->mono_type_arg_names = type_arg_names;
    ti->mono_type_argc = argc;
}

/* =========================================================================
 * Object allocation — bump alloc + set type in XrtArcHdr
 *
 * Uses xrt_arc_alloc (bump allocator) and stores the type_id
 * in XrtArcHdr.type for vtable dispatch and instanceof.
 * ========================================================================= */

static inline void *xrt_obj_alloc(uint16_t type_id, uint32_t size) {
    void *obj = xrt_arc_alloc((size_t) size);
    XrtArcHdr *h = XRT_ARC_HDR(obj);
    h->type = type_id;
    h->flags |= XRT_ARC_HAS_DEINIT;  // mark as having type metadata
    return obj;
}

/* Box an object pointer into XrValue */
static inline XrValue xrt_box_obj(void *obj) {
    XrValue v;
    v.ptr = obj;
    v.tag = obj ? XR_TAG_PTR : XR_TAG_NULL;
    return v;
}

/* Unbox XrValue to object pointer (no type check) */
static inline void *xrt_unbox_obj(XrValue v) {
    return v.ptr;
}

/* =========================================================================
 * instanceof checks — class type identity
 * ========================================================================= */

/* Get display name for a type_id (falls back to internal name) */
static inline const char *xrt_type_display_name(uint16_t type_id) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return "<unknown>";
    const XrtTypeInfo *ti = &xrt_type_table[type_id];
    return ti->display_name ? ti->display_name : ti->name;
}

/* Walk inheritance chain: true if value is an instance of target_tid
 * or any subclass whose parent chain reaches target_tid. Also checks
 * generic_origin at each level for monomorphized classes. */
static inline int xrt_instanceof(XrValue val, uint16_t target_tid) {
    if (val.tag != XR_TAG_PTR || !val.ptr)
        return 0;
    XrtArcHdr *h = XRT_ARC_HDR(val.ptr);
    uint16_t cur = h->type;
    while (cur != 0 && cur < xrt_type_count) {
        if (cur == target_tid)
            return 1;
        if (xrt_type_table[cur].generic_origin == target_tid)
            return 1;
        cur = xrt_type_table[cur].parent_id;
    }
    return 0;
}

#endif  // XRT_CLASS_H
