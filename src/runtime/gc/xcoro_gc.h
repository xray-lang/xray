/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoro_gc.h - Per-Coroutine Immix Mark-Region GC
 *
 * KEY CONCEPT:
 *   - Immix block-line allocator: bump-pointer speed + line-level reclamation
 *   - Objects don't move: C extensions are naturally safe
 *   - Per-coroutine independent GC: no global STW, million concurrency friendly
 *   - Bulk free: release all blocks when coroutine ends
 *   - Incremental GC: Lua GCdebt mechanism, avoid long pauses
 *
 * MEMORY LAYOUT:
 *   1. Value stack (separate allocation, realloc grows)
 *   2. Object heap (Immix blocks, per-block object lists)
 *   3. Large objects (>4KB, separate malloc)
 *
 * GC INVARIANTS:
 *
 *   Objects have three colors: white, gray, and black.
 *   - White: unmarked (may be dead).
 *   - Gray:  marked but children not yet scanned; must be on a gray list.
 *   - Black: marked and all children scanned.
 *
 *   INVARIANT 1 (Tri-color): During PROPAGATE/ATOMIC phases, a black
 *   object must never point to a white object. The write barriers
 *   (forward barrier and back barrier) maintain this invariant.
 *   During SWEEP and PAUSE, this invariant does not hold.
 *
 *   INVARIANT 2 (Gray list): Every gray object must be in exactly one
 *   gray list (gray, grayagain, or weak). This ensures no gray object
 *   is forgotten during traversal.
 *
 *   INVARIANT 3 (White flip): Two white bits alternate between cycles.
 *   currentwhite tracks the "live" white. Dead objects carry the
 *   opposite white ("deadwhite"). New objects are born with currentwhite.
 *   The flip happens in atomic(), after which all unmarkd objects from
 *   the previous cycle become deadwhite and are swept.
 *
 *   INVARIANT 4 (Atomic re-mark): Objects created during PROPAGATE are
 *   born with old currentwhite. The atomic phase re-marks the stack to
 *   catch these objects before the white flip makes them deadwhite.
 *   Without this, live stack objects would be incorrectly swept.
 *
 *   INVARIANT 5 (Shared objects): Shared objects (cross-coroutine) are
 *   not tri-color marked. They use reference counting. The GC records
 *   shared refs each cycle and decrefs objects no longer reachable.
 *
 *   INVARIANT 6 (Immix line marks): alloc_marks tracks which lines in
 *   a block contain live objects. After sweep, alloc_marks reflects
 *   exactly the lines occupied by surviving objects. The allocator only
 *   uses unmarked lines for new allocations.
 *
 *   INVARIANT 7 (tofnz protection): Objects moved to the finalization
 *   list must have their Immix lines protected in alloc_marks. Otherwise,
 *   after swap_marks, the allocator may reuse their lines and overwrite
 *   the object before the finalizer runs.
 *
 * GC STATE MACHINE:
 *
 *   PAUSE ──► PROPAGATE ──► ATOMIC ──► SWEEP ──► PAUSE
 *     │                                              │
 *     └──────────────────────────────────────────────┘
 *
 *   PAUSE:     Idle. GCdebt triggers transition to PROPAGATE.
 *   PROPAGATE: Incremental mark. Pop gray objects, traverse children.
 *              Interruptible (returns to mutator after bounded work).
 *   ATOMIC:    Non-interruptible. Re-mark roots, drain grayagain,
 *              clear weak tables, flip white.
 *   SWEEP:     Non-incremental sweep. Process all blocks and large objects
 *              in one step. Call finalizers inline.
 */

#ifndef XCORO_GC_H
#define XCORO_GC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../../base/xdefs.h"
#include "../../base/xmalloc.h"
#include "../value/xvalue.h"
#include "ximmix.h"
#include "xgc_internal.h"

/* ========== Forward Declarations ========== */

#ifndef XR_VALUE_DEFINED
typedef struct XrValue XrValue;
#endif
struct XrCoroutine;
struct XrGC;

/* ========== Large Object Threshold ========== */

#define XR_LARGE_OBJECT_THRESHOLD (4 * 1024)  // >4KB → large object (xr_malloc)
#define XR_MMAP_THRESHOLD (256 * 1024)        // ≥256KB → mmap (avoid libc heap fragmentation)

/* ========== RC Per-Object Freelist ========== */
/*
 * Small RC-managed objects (≤ XR_LARGE_OBJECT_THRESHOLD) are returned to a
 * per-coroutine segregated freelist when their refcount hits zero, then
 * reused by subsequent allocations of the same size class. This gives RC a
 * real single-object reclamation path on top of the Immix bump allocator,
 * which itself never frees individual objects.
 *
 * Size classes use EXACT 8-byte granularity — the same alignment the
 * allocator rounds every object up to (XGC_ALIGN_SIZE). Exact classes are
 * mandatory: a freed slot's physical footprint is fixed at its first
 * allocation, and newobj overwrites objsize with the new request on reuse.
 * A coarser granularity would let a larger request pop a smaller freed slot
 * and overflow it into the adjacent object. With 8-byte classes every member
 * of a class has the identical aligned size, so reuse is byte-exact.
 *
 * Minimum freelisted size is sizeof(XrGCHeader) + one pointer: the free link
 * is stored in the object's first payload word (header+sizeof(header)), so an
 * object with no payload (header-only, 24 bytes) has nowhere to put it without
 * clobbering the adjacent object. Such objects are not freelisted — they are
 * reclaimed in bulk at coroutine teardown.
 */
#define XR_RC_FREE_GRANULARITY 8 /* must equal XGC_ALIGN_SIZE (allocator alignment) */
#define XR_RC_FREE_MIN_SIZE (sizeof(XrGCHeader) + sizeof(void *)) /* room for the free link */
#define XR_RC_FREECLASSES (XR_LARGE_OBJECT_THRESHOLD / XR_RC_FREE_GRANULARITY)  // 512

/* The freelist size-class step must match the allocator's alignment exactly:
 * every object in a class then has the identical aligned footprint, so a
 * reused slot fits the new request byte-for-byte. If these diverge, a larger
 * allocation could pop a smaller freed slot and overflow the neighbor. */
_Static_assert(XR_RC_FREE_GRANULARITY == XGC_ALIGN_SIZE,
               "RC freelist granularity must equal the GC allocator alignment");

/* Map an aligned allocation size to its freelist class index, or -1 if the
 * size is out of range: too small to hold the free link, or larger than the
 * large-object threshold (those are malloc/mmap-backed, freed individually). */
static inline int xr_rc_size_class(size_t aligned_size) {
    if (aligned_size < XR_RC_FREE_MIN_SIZE || aligned_size > XR_LARGE_OBJECT_THRESHOLD)
        return -1;
    return (int) (aligned_size / XR_RC_FREE_GRANULARITY) - 1;
}

/* ========== Per-Coroutine GC Root Callback ========== */

struct XrCoroGC;
typedef void (*XrCoroGCRootCallback)(struct XrCoroGC *gc, void *userdata);

typedef struct XrCoroGCRootEntry {
    XrCoroGCRootCallback callback;
    void *userdata;
    struct XrCoroGCRootEntry *next;
} XrCoroGCRootEntry;

/* ========== Incremental Sweep Sub-State ========== */

/* ========== Coroutine GC Structure (Immix bump + RC reclamation) ========== */

typedef struct XrCoroGC {
    // === Cache line 0: Immix allocator hot path ===
    // cursor/limit/current_block are the first 3 fields of XrImmixHeap.
    // JIT inline allocation reads cursor/limit at fixed offsets.
    XrImmixHeap immix;

    // === Allocation accounting ===
    int64_t totalbytes;   // Total allocated bytes (gc.count / gc.info stats)
    uint8_t in_gc;        // Re-entry guard (teardown / reset)
    uint8_t gc_disabled;  // gc.disable/enable counter (gc.isrunning)
    uint8_t _pad1[6];     // alignment

    // === Large objects (malloc/mmap-backed; freed individually at teardown) ===
    XrGCHeader *large_objects;  // All large objects (single list via gc_next)
    int64_t large_bytes;        // Total bytes in large_objects

    // GC tuning parameters (gc.setpause / gc.setstepmul — kept for API surface)
    int gc_pause;
    int gc_stepmul;

    // Ownership
    struct XrCoroutine *owner;

    // External root callbacks (legacy C-extension hook; never invoked now that
    // tracing is gone, but the register/unregister API is retained).
    XrCoroGCRootEntry *root_callbacks;

    // Shared-object teardown decref list. Tracing used to populate this each
    // cycle; it is now unused at runtime (shared objects live on the atomic
    // shared-RC) but the teardown decref is retained as a safety net.
    XrGCHeader **shared_refs;
    int shared_refs_count;
    int shared_refs_capacity;
    XrGCHeader **prev_shared_refs;
    int prev_shared_refs_count;

    // Statistics (cold; surfaced by the gc.* builtins)
    uint32_t gc_count;         // Number of explicit gc.collect() calls (no-op cycles)
    uint32_t object_count;     // Live GC object count (incremental counter)
    uint64_t gc_time_ns;       // Cumulative GC time (0 under RC)
    uint64_t last_gc_time_ns;  // Duration of last cycle (0 under RC)
    uint32_t finalizer_count;  // Total finalizers called

    // === RC per-object freelist (RC reclaims small objects) ===
    // Segregated free lists by size class, lazily allocated; on drop-to-zero a
    // small object's memory is pushed here and reused by a later same-class
    // allocation before falling back to Immix bump. NULL until the first free.
    XrGCHeader **rc_freelist;  // array[XR_RC_FREECLASSES] of list heads
} XrCoroGC;

/* ========== JIT Struct Offsets (compile-time constants) ========== */
/* Only the Immix bump fields and totalbytes are read by JIT inline alloc now;
 * the tracing offsets (gcstate / currentwhite / GCdebt / gc_requested) were
 * removed along with the fields. */

#define XR_COROGC_OFFSET_IMMIX offsetof(XrCoroGC, immix)
#define XR_COROGC_OFFSET_TOTALBYTES offsetof(XrCoroGC, totalbytes)

#define XR_IMMIX_OFFSET_CURSOR offsetof(XrImmixHeap, cursor)
#define XR_IMMIX_OFFSET_LIMIT offsetof(XrImmixHeap, limit)

/* ========== Coroutine GC Configuration ========== */

typedef struct XrCoroGCConfig {
    size_t gc_threshold;  // GC trigger threshold (bytes)
    int gc_pause;         // Pause multiplier (100 = collect when memory doubles)
    int gc_stepmul;       // Step multiplier (controls GC speed vs mutator)
} XrCoroGCConfig;

// Main coroutine defaults (long-lived, lower GC pressure)
#define XR_MAIN_CORO_GC_THRESHOLD (8 * 1024 * 1024)  // 8MB
#define XR_MAIN_CORO_GC_PAUSE 200                    // Collect at 200% (more delay)
#define XR_MAIN_CORO_GC_STEPMUL 100                  // Slower GC steps

// Spawn coroutine defaults (short-lived, faster reclaim)
#define XR_SPAWN_CORO_GC_THRESHOLD (32 * 1024)  // 32KB
#define XR_SPAWN_CORO_GC_PAUSE 100              // Collect at 100% (standard)
#define XR_SPAWN_CORO_GC_STEPMUL 200            // Faster GC steps

/* ========== GC Tuning Constants ========== */

// Mark step: bytes of objects to scan per gc_step (debt-proportional)
#define XGC_MARK_STEP_MIN 4096          // Floor: always scan at least 4KB
#define XGC_MARK_STEP_MAX (256 * 1024)  // Cap: never scan > 256KB per step

// Sweep step: blocks per gc_step (debt-proportional)
#define XGC_SWEEP_UNITS_MIN 4    // Floor: at least 4 blocks
#define XGC_SWEEP_UNITS_MAX 128  // Cap: never sweep > 128 blocks

// Adaptive pause bounds (setpause)
#define XGC_PAUSE_MIN 50   // Aggressive GC under memory pressure
#define XGC_PAUSE_MAX 400  // Lazy GC when allocation is slow

// Generational: minor→major promotion trigger (% of GCest)
#define XGC_MAJOR_TRIGGER_PCT 150  // 150% of estimated live → trigger major

// Generational: promotion threshold (live line %)
#define XGC_PROMOTE_THRESHOLD_PCT 40  // ≥40% live lines → promote to old

/* ========== Coroutine GC Lifecycle API ========== */

XR_FUNC XrCoroGC *xr_coro_gc_create(struct XrCoroutine *coro, const XrCoroGCConfig *config);
XR_FUNC void xr_coro_gc_destroy(XrCoroGC *gc);
XR_FUNC void xr_coro_gc_reset(XrCoroGC *gc, struct XrCoroutine *new_owner);

// Flush per-worker GC struct free list (L1) to the isolate-owned
// L2 pool stored on XrSystemHeap. Called from worker destroy to avoid
// struct leaks. Pass `heap=NULL` to force every struct back to malloc.
struct XrSystemHeap;
XR_FUNC void xr_coro_gc_flush_pool(struct XrSystemHeap *heap, struct XrCoroGC **free_list,
                                   int *count);

/* ========== Coroutine GC Allocation API ========== */

/*
 * Core allocation function ()
 * 1. Bump pointer allocate in Arena
 * 2. Link to allgc list
 * 3. Update GCdebt, trigger incremental GC
 */
XR_FUNC XrGCHeader *xr_coro_gc_newobj(XrCoroGC *gc, uint8_t type, size_t size);

/* ========== RC Freelist API ========== */

/* Push a small object's memory onto the RC freelist for its size class.
 * Called by drop-to-zero AFTER the destructor has run. The object's
 * `objsize` must still be valid. No-op for large/region/atomic objects. */
XR_FUNC void xr_coro_gc_rc_free(XrCoroGC *gc, XrGCHeader *obj);

/* drop-to-zero reclamation: run the type destructor (if any) then return
 * the block to the freelist. Routes shared objects to xr_shared_destroy. */
XR_FUNC void xr_coro_gc_rc_destroy(XrCoroGC *gc, XrGCHeader *obj);

/* Release the freelist array itself (block memory is owned by Immix and
 * freed in bulk at coroutine teardown). Called from gc destroy/reset. */
XR_FUNC void xr_coro_gc_rc_freelist_destroy(XrCoroGC *gc);

// Convenience macros
#define xr_coro_gc_new_typed(gc, type, Type)                                                       \
    ((Type *) ((XrGCHeader *) xr_coro_gc_newobj((gc), (type), sizeof(Type)) + 1))

/* ========== Incremental GC API ========== */

/*
 * GC Step: called on each allocation, execute small GC work
 * Lua GCdebt mechanism, amortize GC overhead
 */
XR_FUNC void xr_coro_gc_step(XrCoroGC *gc);

// Full GC cycle
XR_FUNC void xr_coro_gc_fullgc(XrCoroGC *gc);

/* ========== Mark API (retired) ==========
 *
 * Tracing is retired; these are no-ops kept so the traverse helpers and a
 * few callers still compile during the staged removal. They are deleted
 * along with the traverse subsystem. */

XR_FUNC void xr_coro_gc_markobject(XrCoroGC *gc, XrGCHeader *obj);

static inline void xr_coro_gc_markvalue(XrCoroGC *gc, XrValue value) {
    (void) gc;
    (void) value;
}

/* ========== Write Barrier API (retired) ==========
 *
 * Tracing is retired: reference counting owns reclamation, so there is no
 * tri-color invariant to maintain on stores. The write-barrier macros are
 * kept as no-ops so the (numerous) container/instance store sites compile
 * unchanged; ownership of stored values is handled by the compiler-inserted
 * dup/drop (xi_arc) and by container-element drop at destruction. */

// GCHeader from object pointer (GCHeader is first field in all xray objects)
#define XR_OBJ2GC(obj) ((XrGCHeader *) (obj))

#define XR_GC_BARRIER(gc, parent, child) ((void) 0)
#define XR_GC_BARRIER_BACK(gc, obj) ((void) 0)
#define XR_GC_BARRIER_VAL(gc, parent_obj, val) ((void) 0)
#define XR_GC_BARRIER_BACK_SAFE(gc, container_obj) ((void) 0)

/* ========== External Memory Accounting ========== */

/*
 * Notify GC about non-GC malloc'd memory (e.g., array data buffers).
 * Without this, GC has no visibility into external memory pressure
 * and may delay collection when large arrays are abandoned.
 */
static inline void xr_gc_add_external(XrCoroGC *gc, int64_t bytes) {
    if (!gc)
        return;
    gc->totalbytes += bytes;
}

static inline void xr_gc_sub_external(XrCoroGC *gc, int64_t bytes) {
    if (!gc)
        return;
    gc->totalbytes -= bytes;
}

/* ========== Query API ========== */

static inline size_t xr_coro_gc_totalbytes(XrCoroGC *gc) {
    return gc ? (size_t) gc->totalbytes : 0;
}

static inline bool xr_coro_gc_in_gc(XrCoroGC *gc) {
    return gc && gc->in_gc;
}

/* ========== External Root Registration ========== */

/*
 * Register a root callback for this coroutine's GC.
 * Called during mark phase to mark external GC roots (e.g. route closures).
 */
XR_FUNC int xr_coro_gc_register_root(XrCoroGC *gc, XrCoroGCRootCallback callback, void *userdata);

/*
 * Unregister a root callback.
 */
XR_FUNC int xr_coro_gc_unregister_root(XrCoroGC *gc, XrCoroGCRootCallback callback, void *userdata);

/* ========== Debug API ========== */

XR_FUNC void xr_coro_gc_print_stats(XrCoroGC *gc);

#endif  // XCORO_GC_H
