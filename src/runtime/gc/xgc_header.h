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
#include "../value/xtype_names.h"
#include "../../base/xchecks.h"
#include "../../os/os_time.h"

/* ========== GC Debug Options ========== */

#ifndef XR_GC_DEBUG
#define XR_GC_DEBUG 0
#endif

#ifndef XR_GC_STRESS
#define XR_GC_STRESS 0
#endif

#if XR_GC_DEBUG
#define XGC_LOG(fmt, ...) fprintf(stderr, "[XGC] " fmt "\n", ##__VA_ARGS__)
#define XGC_ASSERT(expr) XR_DCHECK(expr, #expr)
#else
#define XGC_LOG(fmt, ...) ((void) 0)
#define XGC_ASSERT(expr) ((void) 0)
#endif  // ========== GC Utility Macros ==========

#define XGC_ALIGN_SIZE 8
#define XGC_ALIGN(size) (((size) + XGC_ALIGN_SIZE - 1) & ~(XGC_ALIGN_SIZE - 1))

// Get current time (nanoseconds)
static inline uint64_t xr_gc_time_ns(void) {
    return xr_time_monotonic_ns();
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
    XR_TINSTANCE,
    XR_TBOUND_METHOD,
    XR_TENUM_TYPE,
    XR_TENUM_VALUE,
    XR_TERROR,
    XR_TMODULE,
    XR_TITERATOR,
    XR_TCOROUTINE,
    XR_TCHANNEL,
    XR_TBIGINT,
    XR_TCOROPOOL,
    XR_TREGEX,
    XR_TBLOB,         // Raw byte buffer on Immix heap (no traverse/destroy)
    XR_TCELL,         // Single-slot mutable capture cell (32B)
    XR_TTASK,         // Lightweight GC-managed coroutine handle (Task/Executor separation)
    XR_TNETCONN,      // Typed TCP / UDP / TLS connection handle (src/io/xnet_handle.h)
    XR_TNETLISTENER,  // Typed TCP listener handle (src/io/xnet_handle.h)
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

#define XR_GC_GET_TYPE(gc) ((XrObjType) ((gc)->type))
#define XR_GC_SET_TYPE(gc, t) ((gc)->type = (uint8_t) (t))
#define XR_GC_GET_MARKED(gc) ((gc)->marked)
#define XR_GC_SET_MARKED(gc, m) ((gc)->marked = (uint8_t) (m))

/* ========== Shared Storage Mode (uses extra field bit 0) ========== */

#define XR_GC_STORAGE_NORMAL 0
#define XR_GC_STORAGE_SHARED 1

#define XR_GC_GET_STORAGE(gc) ((gc)->extra & 0x01)
#define XR_GC_SET_STORAGE(gc, m) ((gc)->extra = ((gc)->extra & ~0x01) | ((m) & 0x01))
#define XR_GC_IS_SHARED(gc) (XR_GC_GET_STORAGE(gc) == XR_GC_STORAGE_SHARED)

/* ========== MMAP Flag (extra field bit 13) ========== */
/*
 * Marks objects allocated via mmap (vs xr_malloc).
 * Used by both system heap (shared objects) and per-coro GC (large objects).
 * Bits 1-12 of extra are now spare (type args moved to XrClass.mono_type_arg_names).
 */
#define XR_GC_FLAG_MMAP 0x2000
#define XR_GC_IS_MMAP(gc) (((gc)->extra & XR_GC_FLAG_MMAP) != 0)
#define XR_GC_SET_MMAP(gc) ((gc)->extra |= XR_GC_FLAG_MMAP)

/* ========== Initialization Functions ========== */

static inline void xr_gc_header_init_type(XrGCHeader *gc, XrObjType type) {
    gc->type = (uint8_t) type;
}

/* ========== Helper Functions ========== */

static inline size_t xr_gc_header_size(void) {
    return sizeof(XrGCHeader);
}
static inline const char *xr_obj_type_name(XrObjType type) {
    static const char *names[] = {TYPE_NAME_NULL,
                                  TYPE_NAME_BOOL,
                                  TYPE_NAME_INT,
                                  TYPE_NAME_FLOAT,
                                  TYPE_NAME_STRING,
                                  TYPE_NAME_FUNCTION,
                                  TYPE_NAME_CFUNCTION,
                                  TYPE_NAME_ARRAY,
                                  TYPE_NAME_SET,
                                  TYPE_NAME_MAP,
                                  TYPE_NAME_CLASS,
                                  TYPE_NAME_INSTANCE,
                                  TYPE_NAME_BOUND_METHOD,
                                  TYPE_NAME_ENUM_TYPE,
                                  TYPE_NAME_ENUM_VALUE,
                                  TYPE_NAME_ERROR,
                                  TYPE_NAME_MODULE,
                                  TYPE_NAME_ITERATOR,
                                  TYPE_NAME_COROUTINE,
                                  TYPE_NAME_CHANNEL,
                                  TYPE_NAME_BIGINT,
                                  TYPE_NAME_COROPOOL,
                                  TYPE_NAME_REGEX,
                                  "blob",
                                  "cell",
                                  TYPE_NAME_TASK,
                                  "NetConn",
                                  "NetListener"};
    _Static_assert(sizeof(names) / sizeof(names[0]) == XR_TNETLISTENER + 1,
                   "xr_obj_type_name: names array out of sync with XrObjType enum");
    if (type < sizeof(names) / sizeof(names[0])) {
        return names[type];
    }
    /* Extension types (allocated dynamically per isolate).
     * Use per-isolate lookup for named types; generic label here. */
    if (type < 64) {
        return "ext";
    }
    return TYPE_NAME_UNKNOWN;
}

#endif  // XGC_HEADER_H
