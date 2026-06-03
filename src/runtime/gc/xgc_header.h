/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xgc_header.h - GC object header definition
 *
 * KEY CONCEPT:
 *   - Unified object header for VM and AOT.
 *   - Class info obtained via type registry, not stored in header.
 *
 * MEMORY LAYOUT — TRANSITIONAL (24 bytes, while tracing GC coexists with RC):
 *   [0-7]   gc_next  (8B) - tracing allgc list (removed when tracing is dropped)
 *   [8]     type     (1B) - object type tag -> destructor dispatch
 *   [9]     marked   (1B) - tracing tri-color + generation (removed with tracing)
 *   [10-11] flags    (2B) - XR_OBJ_* (REGION/ATOMIC/HAS_DTOR/WEAKABLE) + storage/mmap
 *   [12-15] refcount (4B) - compile-time RC; ignored when REGION; atomic when ATOMIC
 *   [16-19] objsize  (4B) - allocation size (region sweep / munmap)
 *   [20-23] _rsv     (4B) - reserved (weak table slot / cycle-report id)
 *
 * The final RC layout removes gc_next and marked, shrinking the header to
 * 16 bytes.
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
    XR_TERROR,
    XR_TMODULE,
    XR_TCOROUTINE,
    XR_TCHANNEL,
    XR_TCOROPOOL,
    XR_TBLOB,    // Raw byte buffer on Immix heap (no traverse/destroy)
    XR_TCELL,    // Single-slot mutable capture cell (32B)
    XR_TTASK,    // Lightweight GC-managed coroutine handle (Task/Executor separation)
    XR_TATOMIC,  // Atomic<T> shared primitive wrapper (lock-free, system heap)
} XrObjType;

/* ========== Unified Object Header (24 bytes, transitional) ==========
 *
 * Transitional layout: tracing fields (gc_next, marked) coexist with the
 * RC field (refcount) until tracing is removed, after which the header
 * shrinks to 16 bytes. The `extra` field doubles as the RC `flags` word
 * (storage/mmap bits today; REGION/ATOMIC/HAS_DTOR/WEAKABLE added for RC).
 */

typedef struct XrGCHeader {
    struct XrGCHeader *gc_next; /* [0-7]  tracing allgc list (removed with tracing) */
    uint8_t type;               /* [8]    object type tag */
    uint8_t marked;             /* [9]    tracing tri-color + generation (removed) */
    uint16_t extra;             /* [10-11] flags word: storage/mmap + XR_OBJ_* */
    int32_t refcount;           /* [12-15] compile-time RC (0 = unmanaged / region) */
    uint32_t objsize;           /* [16-19] allocation size */
    uint32_t _rsv;              /* [20-23] reserved (weak slot / cycle-report id) */
} XrGCHeader;

_Static_assert(sizeof(XrGCHeader) == 24, "XrGCHeader must be 24 bytes (transitional RC+tracing)");

/* Unified alias: the RC memory model refers to the object header as
 * XrObjHeader. During the transition it is the same struct as XrGCHeader. */
typedef struct XrGCHeader XrObjHeader;

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

/* ========== RC Memory-Model Flags (extra field bits 1-4) ==========
 *
 * Bit 0 = storage(shared), bit 13 = mmap (above). Bits 1-4 carry the RC
 * object-model flags. These are populated as the RC model takes over;
 * during the tracing transition they default to 0 (no effect on tracing).
 *
 *   REGION   - object lives in a per-coroutine region: dup/drop are no-ops,
 *              freed in bulk when the coroutine ends.
 *   ATOMIC   - object is shared across coroutines: refcount is atomic.
 *   HAS_DTOR - object's type has a destructor to run at refcount==0.
 *   WEAKABLE - object may be the target of a weak reference.
 */
#define XR_OBJ_REGION 0x0002   /* extra bit 1 */
#define XR_OBJ_ATOMIC 0x0004   /* extra bit 2 */
#define XR_OBJ_HAS_DTOR 0x0008 /* extra bit 3 */
#define XR_OBJ_WEAKABLE 0x0010 /* extra bit 4 */
#define XR_OBJ_DEAD                                                                                \
    0x0020 /* extra bit 5: RC-freed (on freelist); skip                                            \
            * destructor at coroutine teardown to avoid                                            \
            * double finalization. Cleared on reuse. */
#define XR_OBJ_MANAGED                                                                             \
    0x0040 /* extra bit 6: runtime-managed object (Channel / Coroutine /                           \
            * Task / CoroPool / Atomic). Its lifetime is owned by the                              \
            * runtime/scheduler (and the atomic shared-RC in xshared.h),                           \
            * NOT by the compiler-inserted per-coroutine RC. dup/drop are                          \
            * no-ops so a compiler-inserted drop can never free an object                          \
            * the executor still holds. See docs/design/706. */

#define XR_OBJ_GET_FLAG(o, f) (((o)->extra & (f)) != 0)
#define XR_OBJ_SET_FLAG(o, f) ((o)->extra |= (uint16_t) (f))
#define XR_OBJ_CLEAR_FLAG(o, f) ((o)->extra &= (uint16_t) ~(f))

#define XR_OBJ_IS_REGION(o) XR_OBJ_GET_FLAG(o, XR_OBJ_REGION)
#define XR_OBJ_IS_ATOMIC(o) XR_OBJ_GET_FLAG(o, XR_OBJ_ATOMIC)
#define XR_OBJ_HAS_DESTRUCTOR(o) XR_OBJ_GET_FLAG(o, XR_OBJ_HAS_DTOR)
#define XR_OBJ_IS_MANAGED(o) XR_OBJ_GET_FLAG(o, XR_OBJ_MANAGED)

/* Whether an object TYPE is runtime-managed: its lifetime belongs to the
 * runtime/scheduler (cross-coroutine reachable, held by the executor or the
 * atomic shared-RC), not the compiler's per-coroutine RC. The compiler must
 * not insert dup/drop for such objects. See docs/design/706 §5. */
static inline bool xr_objtype_is_runtime_managed(XrObjType t) {
    return t == XR_TCHANNEL || t == XR_TCOROUTINE || t == XR_TTASK || t == XR_TCOROPOOL ||
           t == XR_TATOMIC;
}

/* ========== RC dup/drop Primitives ==========
 *
 * Header-only refcount arithmetic, following Nim's nimIncRef /
 * nimDecRefIsLast contract (lib/system/arc.nim): the primitive only
 * adjusts the count and reports whether the object died; the CALLER runs
 * the destructor and frees, because freeing needs coroutine/region
 * context that does not belong in this header.
 *
 *   - REGION objects: dup/drop are no-ops (freed in bulk at coro end).
 *   - ATOMIC objects: refcount is adjusted atomically (cross-coroutine).
 *   - others: plain non-atomic refcount (the common, fast case).
 *
 * refcount is 1-based here (1 == one owner). xr_obj_drop_is_last returns
 * true exactly when the last owning reference is released.
 */

#include <stdatomic.h>

/* Acquire a new owning reference.
 *
 * REGION and MANAGED objects are no-ops: region objects are bulk-freed at
 * coroutine end, and MANAGED objects (Channel/Coroutine/Task/CoroPool/Atomic)
 * are owned by the runtime/scheduler via the atomic shared-RC, never by the
 * compiler-inserted per-coroutine RC. See docs/design/706 §5. */
static inline void xr_obj_dup(XrObjHeader *o) {
    if (!o || (o->extra & (XR_OBJ_REGION | XR_OBJ_MANAGED)))
        return;
    if (o->extra & XR_OBJ_ATOMIC)
        atomic_fetch_add_explicit((_Atomic(int32_t) *) &o->refcount, 1, memory_order_relaxed);
    else
        o->refcount++;
}

/* Release an owning reference. Returns true if this was the last reference
 * (refcount reached 0) and the caller must destroy + free the object.
 *
 * REGION and MANAGED objects always return false: a compiler-inserted drop
 * must never free a region object (bulk-freed at coroutine end) nor a
 * runtime-managed object the executor still holds. See docs/design/706 §5. */
static inline bool xr_obj_drop_is_last(XrObjHeader *o) {
    if (!o || (o->extra & (XR_OBJ_REGION | XR_OBJ_MANAGED)))
        return false; /* region: bulk-freed at coro end; managed: runtime-owned */
    if (o->extra & XR_OBJ_ATOMIC) {
        /* acq_rel so the destructor sees all prior writes (Rust Arc pattern). */
        return atomic_fetch_sub_explicit((_Atomic(int32_t) *) &o->refcount, 1,
                                         memory_order_acq_rel) == 1;
    }
    return --o->refcount == 0;
}

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
                                  TYPE_NAME_ERROR,
                                  TYPE_NAME_MODULE,
                                  TYPE_NAME_COROUTINE,
                                  TYPE_NAME_CHANNEL,
                                  TYPE_NAME_COROPOOL,
                                  "blob",
                                  "cell",
                                  TYPE_NAME_TASK,
                                  TYPE_NAME_ATOMIC};
    _Static_assert(sizeof(names) / sizeof(names[0]) == XR_TATOMIC + 1,
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
