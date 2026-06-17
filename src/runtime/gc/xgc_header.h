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
 * MEMORY LAYOUT (16 bytes):
 *   [0-1]   type     (2B) - object type tag -> destructor dispatch
 *   [2-3]   flags    (2B) - XR_OBJ_* (REGION/ATOMIC/HAS_DTOR/WEAKABLE) + storage/mmap
 *   [4-7]   refcount (4B) - 0-based sign-tagged RC (see "Signed RC Encoding")
 *   [8-11]  objsize  (4B) - allocation size (region sweep / munmap)
 *   [12-15] _rsv     (4B) - reserved (weak table slot / cycle-report id)
 */

#ifndef XGC_HEADER_H
#define XGC_HEADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../value/xtype_names.h"
#include "../../base/xchecks.h"
#include "../../os/os_time.h"
#include "../../shared/xr_obj_header.h" /* unified XrGCHeader/XrObjHeader + HAS_DTOR/STORAGE_BUMP */

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
    XR_TBLOB,         // Raw byte buffer on Region heap (no traverse/destroy)
    XR_TCELL,         // Single-slot mutable capture cell (32B)
    XR_TTASK,         // Lightweight GC-managed coroutine handle (Task/Executor separation)
    XR_TATOMIC,       // Atomic<T> shared primitive wrapper (lock-free, system heap)
    XR_TWORKQUEUE,    // WorkQueue<T> shared sharded queue (system heap)
    XR_TRESULTGROUP,  // ResultGroup shared scalar reducer (system heap)
} XrObjType;

/* Unified object header (XrGCHeader / XrObjHeader, 16 bytes) +
 * XR_OBJ_HAS_DTOR / XR_OBJ_STORAGE_BUMP now live in the self-contained
 * src/shared/xr_obj_header.h (included above) so the AOT runtime can adopt the
 * same layout. The remaining flag bits and accessors below share its bit space. */

/* ========== Access Macros ========== */

#define XR_GC_GET_TYPE(gc) ((XrObjType) ((gc)->type))
#define XR_GC_SET_TYPE(gc, t) ((gc)->type = (uint16_t) (t))

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
 * object-model flags.
 *
 *   REGION   - object lives in a per-coroutine region: dup/drop are no-ops,
 *              freed in bulk when the coroutine ends.
 *   ATOMIC   - object is shared across coroutines: refcount is atomic.
 *   HAS_DTOR - object's type has a destructor to run at refcount==0.
 *   WEAKABLE - object may be the target of a weak reference.
 */
#define XR_OBJ_REGION 0x0002 /* extra bit 1 */
#define XR_OBJ_ATOMIC 0x0004 /* extra bit 2 */
/* XR_OBJ_HAS_DTOR (bit 3) is defined in src/shared/xr_obj_header.h */
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
            * the executor still holds. */

#define XR_OBJ_CYCLE_CANDIDATE                                                                     \
    0x0080 /* extra bit 7: type may participate in reference cycles.                               \
            * Set at allocation for instances whose class forms a cycle in                         \
            * the compile-time reference graph. On RC decrement to > 0 the                         \
            * object is added to per-coroutine cycle_roots for trial deletion. */

/* extra bits 8-9: trial-deletion color used by the cycle collector
 * (xcycle_gc.c). Living here instead of _rsv so the collector never
 * disturbs the _rsv root-index/sentinel invariant. Owner-thread only:
 * the collector runs on the coroutine's own worker. */
#define XR_OBJ_CYCLE_COLOR_MASK 0x0300
#define XR_OBJ_CYCLE_COLOR_SHIFT 8

#define XR_OBJ_TRANSIT                                                                             \
    0x0400 /* extra bit 10: channel-transit copy. The object graph is a                            \
            * coroutine-independent deep copy made by the send side; the                           \
            * channel buffer owns one atomic reference. The receive side                           \
            * deep-copies it into the receiver's heap (TRANSIT is never                            \
            * pointer-shared like other SHARED objects) and then drops                             \
            * the buffer reference, freeing the whole graph. */

#define XR_OBJ_GET_FLAG(o, f) (((o)->extra & (f)) != 0)
#define XR_OBJ_SET_FLAG(o, f) ((o)->extra |= (uint16_t) (f))
#define XR_OBJ_CLEAR_FLAG(o, f) ((o)->extra &= (uint16_t) ~(f))

#define XR_OBJ_IS_REGION(o) XR_OBJ_GET_FLAG(o, XR_OBJ_REGION)
#define XR_OBJ_IS_ATOMIC(o) XR_OBJ_GET_FLAG(o, XR_OBJ_ATOMIC)
#define XR_OBJ_HAS_DESTRUCTOR(o) XR_OBJ_GET_FLAG(o, XR_OBJ_HAS_DTOR)
#define XR_OBJ_IS_MANAGED(o) XR_OBJ_GET_FLAG(o, XR_OBJ_MANAGED)

/* Whether an object TYPE is runtime-managed: its lifetime belongs to the
 * scheduler/executor, not the compiler's per-coroutine RC. Only Coroutine,
 * Task, and CoroPool qualify (the executor holds them past the code handle's
 * death). Channel, Atomic, WorkQueue, and ResultGroup are pure shared DATA and
 * use the atomic shared-RC like `shared const` (compiler-tracked, last drop
 * frees). The authoritative per-instance signal is the XR_OBJ_MANAGED flag,
 * set only on Coroutine/Task and on the timer-channel variant (whose embedded
 * node the timer wheel owns asynchronously). */
static inline bool xr_objtype_is_runtime_managed(XrObjType t) {
    return t == XR_TCOROUTINE || t == XR_TTASK || t == XR_TCOROPOOL;
}

/* ========== Signed RC Encoding ==========
 *
 * The refcount field is a 0-based, sign-tagged count shared verbatim by the
 * VM and AOT runtimes. The sign routes every object to one of two
 * paths with a single load and a single branch — the property that keeps
 * dup/drop down to a handful of instructions (Koka's kklib
 * signed scheme, refcount.c, inspired this).
 *
 *   rc >= 0   THREAD-LOCAL (non-atomic, the common fast path).
 *             Live references = rc + 1, so rc == 0 means UNIQUE (exactly one
 *             owner; a drop frees it and it is eligible for in-place reuse).
 *               dup : rc++
 *               drop: rc == 0 ? free : rc--
 *
 *   rc < 0    SLOW PATH (cold), resolved by value/flags:
 *             - rc <= XR_RC_STICKY_BAND : immortal / region / fixed object.
 *               dup and drop are no-ops (RC never frees it).
 *             - otherwise (atomic band) : THREAD-SHARED, references = -rc.
 *                 * MANAGED objects (Channel/Coroutine/Task/...) hand their
 *                   lifetime to the runtime/scheduler; the compiler-inserted
 *                   dup/drop are no-ops (the runtime counts via xshared.h).
 *                 * other shared objects (shared const) are inc/dec'd
 *                   atomically: more-negative = more refs; the last drop
 *                   (old == -1) frees.
 *
 * The primitive only adjusts the count and reports whether the object died;
 * the CALLER runs the destructor and frees, because freeing needs the
 * coroutine context that does not belong in this header (Nim arc.nim
 * nimDecRefIsLast contract).
 */

#include <stdatomic.h>

/* XR_RC_STICKY / XR_RC_STICKY_BAND / XR_RC_INIT live in
 * src/shared/xr_obj_header.h so standalone AOT bump objects can use the same
 * sentinel as the VM/AOT RC fast paths. */

static inline bool xr_rc_is_sticky(int32_t rc) {
    return rc <= XR_RC_STICKY_BAND;
}

/* Cold path for dup on a non-thread-local object (rc < 0). */
static inline void xr_obj_dup_slow(XrObjHeader *o) {
    if (o->extra & XR_OBJ_STORAGE_BUMP)
        return; /* bump arena: freed in bulk, never counted */
    _Atomic(int32_t) *rcp = &o->refcount;
    int32_t rc = atomic_load_explicit(rcp, memory_order_relaxed);
    if (xr_rc_is_sticky(rc))
        return; /* immortal / region: never counted */
    if (o->extra & XR_OBJ_MANAGED)
        return; /* runtime-owned: the compiler must not touch the count */
    /* thread-shared atomic: references = -rc, so a new reference moves the
     * count one step more negative; saturate to immortal near the band. */
    int32_t old = atomic_fetch_sub_explicit(rcp, 1, memory_order_relaxed);
    if (old - 1 <= XR_RC_STICKY_BAND)
        atomic_store_explicit(rcp, XR_RC_STICKY, memory_order_relaxed);
}

/* Cold path for drop on a non-thread-local object (rc < 0). Returns true
 * when the last shared reference is released and the caller must destroy. */
static inline bool xr_obj_drop_slow_is_last(XrObjHeader *o) {
    if (o->extra & XR_OBJ_STORAGE_BUMP)
        return false; /* bump arena: freed in bulk, never RC-freed */
    _Atomic(int32_t) *rcp = &o->refcount;
    int32_t rc = atomic_load_explicit(rcp, memory_order_relaxed);
    if (xr_rc_is_sticky(rc))
        return false; /* immortal / region: bulk-freed, never RC-freed */
    if (o->extra & XR_OBJ_MANAGED)
        return false; /* runtime-owned */
    /* thread-shared atomic: references = -rc; releasing moves toward zero.
     * acq_rel so the destructor observes all prior writes (Rust Arc). */
    int32_t old = atomic_fetch_add_explicit(rcp, 1, memory_order_acq_rel);
    return old == -1; /* -1 == exactly one reference → now zero */
}

/* Acquire a new owning reference. Thread-local fast path is a plain inc;
 * shared/managed/immortal objects (rc < 0) take the cold path. */
static inline void xr_obj_dup(XrObjHeader *o) {
    if (!o)
        return;
    if (o->extra & XR_OBJ_STORAGE_BUMP)
        return; /* bump arena: freed in bulk, never counted */
    int32_t rc = atomic_load_explicit(&o->refcount, memory_order_relaxed);
    if (rc >= 0) {
        atomic_store_explicit(&o->refcount, rc + 1, memory_order_relaxed);
        return;
    }
    xr_obj_dup_slow(o);
}

/* Release an owning reference. Returns true if this was the last reference
 * and the caller must destroy + free the object. Thread-local fast path is a
 * plain compare + dec; rc == 0 (unique) reports death; rc < 0 is cold. */
static inline bool xr_obj_drop_is_last(XrObjHeader *o) {
    if (!o)
        return false;
    if (o->extra & XR_OBJ_STORAGE_BUMP)
        return false; /* bump arena: freed in bulk, never RC-freed */
    int32_t rc = atomic_load_explicit(&o->refcount, memory_order_relaxed);
    if (rc > 0) {
        atomic_store_explicit(&o->refcount, rc - 1, memory_order_relaxed);
        return false;
    }
    if (rc == 0)
        return true;
    return xr_obj_drop_slow_is_last(o);
}

/* Whether a thread-local object is uniquely owned (drop-reuse eligibility).
 * Shared/immortal objects (rc < 0) are never unique to a single coroutine. */
static inline bool xr_obj_is_unique(const XrObjHeader *o) {
    return o && !(o->extra & XR_OBJ_STORAGE_BUMP) &&
           atomic_load_explicit(&o->refcount, memory_order_relaxed) == 0;
}

/* ========== Initialization Functions ========== */

static inline void xr_gc_header_init_type(XrGCHeader *gc, XrObjType type) {
    gc->type = (uint16_t) type;
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
                                  TYPE_NAME_ATOMIC,
                                  TYPE_NAME_WORKQUEUE,
                                  TYPE_NAME_RESULTGROUP};
    _Static_assert(sizeof(names) / sizeof(names[0]) == XR_TRESULTGROUP + 1,
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
