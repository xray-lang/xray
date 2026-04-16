/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_header.h - GC object header definition (16 bytes)
 *
 * KEY CONCEPT:
 *   - 16-byte compact header
 *   - Class info obtained via type registry, not stored in GC header
 *
 * MEMORY LAYOUT (16 bytes):
 *   [0-7]   gc_next (8B) - GC global list
 *   [8]     type (1B)    - Object type
 *   [9]     marked (1B)  - GC color + age
 *   [10-11] extra (2B)   - Type-specific data
 *   [12-15] objsize (4B) - Object allocation size
 */

#ifndef XGC_HEADER_H
#define XGC_HEADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "../value/xtype_names.h"
#include "../../base/xchecks.h"

/* ========== GC Debug Options ========== */

#ifndef XR_GC_DEBUG
#define XR_GC_DEBUG 0
#endif

#if XR_GC_DEBUG
#define XGC_LOG(fmt, ...) \
    fprintf(stderr, "[XGC] " fmt "\n", ##__VA_ARGS__)
#define XGC_ASSERT(expr) XR_DCHECK(expr, #expr)
#else
#define XGC_LOG(fmt, ...) ((void)0)
#define XGC_ASSERT(expr) ((void)0)
#endif // ========== GC Utility Macros ==========

#define XGC_ALIGN_SIZE 8
#define XGC_ALIGN(size) \
    (((size) + XGC_ALIGN_SIZE - 1) & ~(XGC_ALIGN_SIZE - 1))

// Get current time (nanoseconds)
static inline uint64_t xr_gc_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Forward declarations
struct XrClass;
typedef struct XrClass XrClass;

/* ========== Object Type Definition ========== */

typedef enum {
    XR_TNULL = 0,
    XR_TBOOL,
    XR_TINT,
    XR_TFLOAT,
    XR_TSTRING,
    XR_TFUNCTION,
    XR_TCFUNCTION,
    XR_TARRAY,
    XR_TSET,
    XR_TMAP,
    XR_TCLASS,
    XR_TCLASS_BUILDER,
    XR_TINSTANCE,
    XR_TBOUND_METHOD,
    XR_TENUM_TYPE,
    XR_TENUM_VALUE,
    XR_TERROR,
    XR_TEXCEPTION,
    XR_TMODULE,
    XR_TITERATOR,
    XR_TRESERVED,
    XR_TSTRINGBUILDER,
    XR_TJSON,
    XR_TSHAPE,
    XR_TCOROUTINE,
    XR_TCHANNEL,
    XR_TBIGINT,
    XR_TCOROPOOL,
    XR_TARRAY_SLICE,
    XR_TDATETIME,
    XR_TREGEX,
    XR_TLOGGER,
    XR_TRANGE,
    XR_TBLOB,           // Raw byte buffer on Immix heap (no traverse/destroy)
    XR_TCONTEXT,        // Closure context object (captured variable storage)
    XR_TCELL,           // Single-slot mutable capture cell (32B, replaces 1-slot XrContext)
    XR_TTASK,           // Lightweight GC-managed coroutine handle (Task/Executor separation)
} XrObjType;

/* ========== Unified GC Header (16 bytes) ========== */

typedef struct XrGCHeader {
    struct XrGCHeader *gc_next;
    uint8_t type;
    uint8_t marked;
    uint16_t extra;
    uint32_t objsize;
} XrGCHeader;

_Static_assert(sizeof(XrGCHeader) == 16, "XrGCHeader must be 16 bytes");

/* ========== Access Macros ========== */

#define XR_GC_GET_TYPE(gc)      ((XrObjType)((gc)->type))
#define XR_GC_SET_TYPE(gc, t)   ((gc)->type = (uint8_t)(t))
#define XR_GC_GET_MARKED(gc)    ((gc)->marked)
#define XR_GC_SET_MARKED(gc, m) ((gc)->marked = (uint8_t)(m))

/* ========== Shared Storage Mode (uses extra field bit 0) ========== */

#define XR_GC_STORAGE_NORMAL  0
#define XR_GC_STORAGE_SHARED  1

#define XR_GC_GET_STORAGE(gc)      ((gc)->extra & 0x01)
#define XR_GC_SET_STORAGE(gc, m)   ((gc)->extra = ((gc)->extra & ~0x01) | ((m) & 0x01))
#define XR_GC_IS_SHARED(gc)        (XR_GC_GET_STORAGE(gc) == XR_GC_STORAGE_SHARED)

/* ========== Instance Reified Type Args (uses extra bits 1-12) ========== */
/*
 * Layout: [15:13 spare][12:8 tid1][7:3 tid0][2:1 argc][0 storage]
 *   argc: 0-3 type arguments
 *   tid0/tid1: XrTypeId (5 bits each, 0-31)
 */
#define XR_INST_TYPE_ARGC(gc)      (((gc)->extra >> 1) & 0x03)
#define XR_INST_TYPE_ARG0(gc)      (((gc)->extra >> 3) & 0x1F)
#define XR_INST_TYPE_ARG1(gc)      (((gc)->extra >> 8) & 0x1F)
#define XR_INST_SET_TYPE_ARGS(gc, argc, tid0, tid1) \
    ((gc)->extra = ((gc)->extra & 0x01) | \
        (((argc) & 0x03) << 1) | (((tid0) & 0x1F) << 3) | (((tid1) & 0x1F) << 8))

/* ========== Initialization Functions ========== */

static inline void xr_gc_header_init_type(XrGCHeader *gc, XrObjType type) {
    gc->type = (uint8_t)type;
}

/* ========== Helper Functions ========== */

static inline size_t xr_gc_header_size(void) {
    return sizeof(XrGCHeader);
}
static inline const char* xr_obj_type_name(XrObjType type) {
    static const char* names[] = {
        TYPE_NAME_NULL, TYPE_NAME_BOOL, TYPE_NAME_INT, TYPE_NAME_FLOAT, TYPE_NAME_STRING,
        TYPE_NAME_FUNCTION, TYPE_NAME_CFUNCTION,
        TYPE_NAME_ARRAY, TYPE_NAME_SET, TYPE_NAME_MAP,
        TYPE_NAME_CLASS, TYPE_NAME_CLASS_BUILDER, TYPE_NAME_INSTANCE, TYPE_NAME_BOUND_METHOD,
        TYPE_NAME_ENUM_TYPE, TYPE_NAME_ENUM_VALUE,
        TYPE_NAME_ERROR, TYPE_NAME_EXCEPTION, TYPE_NAME_MODULE, TYPE_NAME_ITERATOR, "reserved",
        TYPE_NAME_STRINGBUILDER, TYPE_NAME_JSON, TYPE_NAME_SHAPE,
        TYPE_NAME_COROUTINE, TYPE_NAME_CHANNEL, TYPE_NAME_BIGINT, TYPE_NAME_COROPOOL,
        TYPE_NAME_ARRAY_SLICE,
        TYPE_NAME_DATETIME, TYPE_NAME_REGEX,
        TYPE_NAME_LOGGER, TYPE_NAME_RANGE, "blob", "context", "cell", TYPE_NAME_TASK
    };
    _Static_assert(sizeof(names)/sizeof(names[0]) == XR_TTASK + 1,
                   "xr_obj_type_name: names array out of sync with XrObjType enum");
    if (type < sizeof(names)/sizeof(names[0])) {
        return names[type];
    }
    return TYPE_NAME_UNKNOWN;
}

#endif // XGC_HEADER_H
