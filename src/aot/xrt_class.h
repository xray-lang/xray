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
 *   All heap objects (class instances, promoted structs) carry XrObjHeader
 *   from xrt_arc.h as a common header.  XrObjHeader.type indexes into
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
 *   _tid_Point = xrt_type_register_hot(0, NULL, 0, NULL, nfields*16);
 *   xrt_type_set_name(_tid_Point, "Point", NULL);
 *
 *   // --- constructor call ---
 *   { XrValue _inst = xrt_box_obj(xrt_obj_alloc(_tid_Point, nfields*16));
 *     xr_constructor(xrt_ctx, _inst, ...);
 *     v5 = _inst; }
 *
 * RELATED MODULES:
 *   - xrt_arc.h: XrObjHeader, bump allocator
 *   - xrt_value.h: XrValue tagged union (PTR tag carries object pointer)
 *   - xi_cgen.c: emits class type registration and constructor calls
 */

#ifndef XRT_CLASS_H
#define XRT_CLASS_H

#include "xrt_value.h"
#include "xrt_arc.h"  // XrObjHeader, XRT_ARC_HDR, xrt_arc_alloc, macros
#include "../shared/xr_derive_flags.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

/* =========================================================================
 * Type Info — one hot entry plus optional cold name/derive entries per
 * class/struct type. XrObjHeader.type indexes into xrt_type_table[].
 * ========================================================================= */

typedef void (*XrtDestructor)(void *obj);
typedef XrValue (*XrtMethodFn)(void);  // generic fn ptr placeholder

typedef struct {
    const char *name;
    uint16_t offset;
    uint8_t native_type;
} XrtInspectField;

typedef struct {
    uint16_t type_id;
    uint16_t parent_id;       // 0 = no parent
    uint16_t generic_origin;  // type_id of skeleton class; 0 = not monomorphized
    XrtMethodFn *vtable;      // virtual method table (NULL if no virtuals)
    int vtable_size;
    XrtDestructor destructor;  // NULL for classes without custom dtor
    uint32_t instance_size;    // byte size of instance fields
} XrtTypeInfo;

typedef struct {
    const char *name;                  // internal name (e.g. "Box$i64")
    const char *display_name;          // user-visible name (e.g. "Box"); NULL = same as name
    const char **mono_type_arg_names;  // static array of display name strings, NULL if not generic
    uint8_t mono_type_argc;            // element count of mono_type_arg_names
} XrtTypeNameInfo;

typedef struct {
    uint32_t derive_flags;  // XR_DERIVE_* opt-in metadata flags
    const XrtInspectField *inspect_fields;
    uint16_t inspect_field_count;
} XrtTypeDeriveInfo;

/* =========================================================================
 * Type table — populated by generated code at module init
 *
 * Index 0 is reserved (no type / null).
 * Max 256 types in a single AOT binary for now.
 * ========================================================================= */

/* Initial capacity; the table grows on demand (xrt_type_register) up to the
 * uint16_t type-id space (65535), so an AOT binary is not capped at 256 types. */
#define XRT_INIT_TYPES 256

#ifdef XRT_IMPL
XrtTypeInfo *xrt_type_table = NULL;
XrtTypeNameInfo *xrt_type_name_table = NULL;
XrtTypeDeriveInfo *xrt_type_derive_table = NULL;
uint16_t xrt_type_count = 1;  // 0 reserved
uint16_t xrt_type_cap = 0;
#else
extern XrtTypeInfo *xrt_type_table;
extern XrtTypeNameInfo *xrt_type_name_table;
extern XrtTypeDeriveInfo *xrt_type_derive_table;
extern uint16_t xrt_type_count;
extern uint16_t xrt_type_cap;
#endif

/* Register hot type identity; returns assigned type_id. Names are optional cold
 * metadata installed separately by xrt_type_set_name()/xrt_type_set_generic_name(). */
static inline uint16_t xrt_type_register_hot(uint16_t parent_id, XrtMethodFn *vtable,
                                             int vtable_size, XrtDestructor dtor,
                                             uint32_t inst_size) {
    if (xrt_type_count >= xrt_type_cap) {
        uint32_t nc = xrt_type_cap ? (uint32_t) xrt_type_cap * 2u : (uint32_t) XRT_INIT_TYPES;
        if (nc > 0xFFFFu)
            nc = 0xFFFFu; /* type_id is uint16_t */
        if ((uint32_t) xrt_type_count >= nc) {
            fprintf(stderr, "xrt_type_register_hot: type table full (max %u types)\n",
                    (unsigned) nc);
            abort();
        }
        XrtTypeInfo *nt =
            (XrtTypeInfo *) XRT_REALLOC(xrt_type_table, (size_t) nc * sizeof(XrtTypeInfo));
        XrtTypeNameInfo *nn = (XrtTypeNameInfo *) XRT_REALLOC(
            xrt_type_name_table, (size_t) nc * sizeof(XrtTypeNameInfo));
        XrtTypeDeriveInfo *nd = (XrtTypeDeriveInfo *) XRT_REALLOC(
            xrt_type_derive_table, (size_t) nc * sizeof(XrtTypeDeriveInfo));
        if (!nt || !nn || !nd) {
            fprintf(stderr, "xrt_type_register_hot: out of memory growing type table\n");
            abort();
        }
        memset(&nt[xrt_type_cap], 0, ((size_t) nc - (size_t) xrt_type_cap) * sizeof(XrtTypeInfo));
        memset(&nn[xrt_type_cap], 0,
               ((size_t) nc - (size_t) xrt_type_cap) * sizeof(XrtTypeNameInfo));
        memset(&nd[xrt_type_cap], 0,
               ((size_t) nc - (size_t) xrt_type_cap) * sizeof(XrtTypeDeriveInfo));
        xrt_type_table = nt;
        xrt_type_name_table = nn;
        xrt_type_derive_table = nd;
        xrt_type_cap = (uint16_t) nc;
    }
    uint16_t id = xrt_type_count++;
    XrtTypeInfo *ti = &xrt_type_table[id];
    XrtTypeNameInfo *ni = &xrt_type_name_table[id];
    XrtTypeDeriveInfo *di = &xrt_type_derive_table[id];
    ti->type_id = id;
    ti->parent_id = parent_id;
    ti->generic_origin = 0;
    ti->vtable = vtable;
    ti->vtable_size = vtable_size;
    ti->destructor = dtor;
    ti->instance_size = inst_size;
    ni->name = NULL;
    ni->display_name = NULL;
    ni->mono_type_arg_names = NULL;
    ni->mono_type_argc = 0;
    di->derive_flags = 0;
    di->inspect_fields = NULL;
    di->inspect_field_count = 0;
    return id;
}

static inline void xrt_type_set_name(uint16_t type_id, const char *name, const char *display) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return;
    XrtTypeNameInfo *ni = &xrt_type_name_table[type_id];
    ni->name = name;
    ni->display_name = display;
}

static inline void xrt_type_set_generic_origin(uint16_t type_id, uint16_t origin_id) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return;
    XrtTypeInfo *ti = &xrt_type_table[type_id];
    ti->generic_origin = origin_id;
}

/* Set display metadata for a monomorphized type.
 * type_arg_names is a static array of string literals (no ownership transfer). */
static inline void xrt_type_set_generic_name(uint16_t type_id, const char *display,
                                             const char **type_arg_names, uint8_t argc) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return;
    XrtTypeNameInfo *ni = &xrt_type_name_table[type_id];
    ni->display_name = display;
    ni->mono_type_arg_names = type_arg_names;
    ni->mono_type_argc = argc;
}

static inline void xrt_type_set_derive(uint16_t type_id, uint32_t derive_flags,
                                       const XrtInspectField *inspect_fields,
                                       uint16_t inspect_field_count) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return;
    XrtTypeDeriveInfo *di = &xrt_type_derive_table[type_id];
    di->derive_flags = derive_flags;
    di->inspect_fields = inspect_fields;
    di->inspect_field_count = inspect_fields ? inspect_field_count : 0;
}

static inline const XrtTypeInfo *xrt_type_info(uint16_t type_id) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return NULL;
    return &xrt_type_table[type_id];
}

static inline const XrtTypeNameInfo *xrt_type_name_info(uint16_t type_id) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return NULL;
    return &xrt_type_name_table[type_id];
}

static inline const XrtTypeDeriveInfo *xrt_type_derive_info(uint16_t type_id) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return NULL;
    return &xrt_type_derive_table[type_id];
}

static inline XrValue xrt_inspect_field_value(const void *obj, const XrtInspectField *field) {
    if (!obj || !field)
        return XR_NULL_VAL;
    const uint8_t *p = (const uint8_t *) obj + field->offset;
    switch (field->native_type) {
        case XR_NATIVE_I64:
            return XR_FROM_INT(*(const int64_t *) p);
        case XR_NATIVE_F64:
            return XR_FROM_FLOAT(*(const double *) p);
        case XR_NATIVE_BOOL:
            return XR_FROM_BOOL(*(const uint8_t *) p != 0);
        case XR_NATIVE_I8:
            return XR_FROM_INT(*(const int8_t *) p);
        case XR_NATIVE_I16:
            return XR_FROM_INT(*(const int16_t *) p);
        case XR_NATIVE_I32:
            return XR_FROM_INT(*(const int32_t *) p);
        case XR_NATIVE_U8:
            return XR_FROM_INT(*(const uint8_t *) p);
        case XR_NATIVE_U16:
            return XR_FROM_INT(*(const uint16_t *) p);
        case XR_NATIVE_U32:
            return XR_FROM_INT(*(const uint32_t *) p);
        case XR_NATIVE_U64:
            return XR_FROM_INT((int64_t) *(const uint64_t *) p);
        case XR_NATIVE_ISIZE:
            return XR_FROM_INT((int64_t) *(const ptrdiff_t *) p);
        case XR_NATIVE_USIZE:
            return XR_FROM_INT((int64_t) *(const size_t *) p);
        case XR_NATIVE_F32:
            return XR_FROM_FLOAT((double) *(const float *) p);
        case XR_NATIVE_STRING:
        case XR_NATIVE_VALUE:
            return *(const XrValue *) p;
        case XR_NATIVE_ARRAY_REF: {
            void *ptr = *(void *const *) p;
            return ptr ? xr_mkptr(ptr, XR_TAG_ARRAY) : XR_NULL_VAL;
        }
        case XR_NATIVE_MAP_REF: {
            void *ptr = *(void *const *) p;
            return ptr ? xr_mkptr(ptr, XR_TAG_MAP) : XR_NULL_VAL;
        }
        case XR_NATIVE_SET_REF: {
            void *ptr = *(void *const *) p;
            return ptr ? xr_mkptr(ptr, XR_TAG_SET) : XR_NULL_VAL;
        }
        default:
            return XR_NULL_VAL;
    }
}

/* Run the type-specific destructor for an object, if its type registered
 * one. Called from xrt_release (xrt_arc.h) on the last reference, before the
 * block is freed. Forward-declared in xrt_arc.h. */
static inline void xrt_dispatch_destructor(uint16_t type_id, void *obj) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return;
    XrtDestructor dtor = xrt_type_table[type_id].destructor;
    if (dtor)
        dtor(obj);
}

/* =========================================================================
 * Object allocation — bump alloc + set type in XrObjHeader
 *
 * Uses xrt_arc_alloc (bump allocator) and stores the type_id
 * in XrObjHeader.type for vtable dispatch and instanceof.
 * ========================================================================= */

static inline void *xrt_obj_alloc(uint16_t type_id, uint32_t size) {
    void *obj = xrt_arc_alloc((size_t) size);
    XrObjHeader *h = XRT_ARC_HDR(obj);
    h->type = type_id;
    h->extra |= XR_OBJ_HAS_DTOR;  // mark as having type metadata
    return obj;
}

/* Box an object pointer into XrValue */
static inline XrValue xrt_box_obj(void *obj) {
    return obj ? xr_mkheap(obj, XR_TINSTANCE) : XR_NULL_VAL;
}

/* Unbox XrValue to object pointer (no type check) */
static inline void *xrt_unbox_obj(XrValue v) {
    return v.ptr;
}

/* =========================================================================
 * instanceof checks — class type identity
 * ========================================================================= */

/* Get display name for a type_id (falls back to internal name). Returns NULL
 * when the build profile stripped user type-name metadata. */
static inline const char *xrt_type_display_name(uint16_t type_id) {
    if (type_id == 0 || type_id >= xrt_type_count)
        return NULL;
    const XrtTypeNameInfo *ni = &xrt_type_name_table[type_id];
    return ni->display_name ? ni->display_name : ni->name;
}

static inline int xrt_type_internal_name_eq(uint16_t type_id, const char *name) {
    if (type_id == 0 || type_id >= xrt_type_count || !name)
        return 0;
    const XrtTypeNameInfo *ni = &xrt_type_name_table[type_id];
    return ni->name && strcmp(ni->name, name) == 0;
}

static inline int xrt_instance_exact_type(XrValue val, uint16_t expected_tid) {
    if (expected_tid == 0 || val.tag != XR_TAG_PTR || val.heap_type != XR_TINSTANCE || !val.ptr)
        return 0;
    XrObjHeader *h = XRT_ARC_HDR(val.ptr);
    return h->type == expected_tid;
}

/* Walk inheritance chain: true if value is an instance of target_tid
 * or any subclass whose parent chain reaches target_tid. Also checks
 * generic_origin at each level for monomorphized classes. */
static inline int xrt_instanceof(XrValue val, uint16_t target_tid) {
    if (val.tag != XR_TAG_PTR || val.heap_type != XR_TINSTANCE || !val.ptr)
        return 0;
    XrObjHeader *h = XRT_ARC_HDR(val.ptr);
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
