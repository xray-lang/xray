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

/* ========== GC State (4-State Machine) ========== */

#define XGC_PAUSE 0      // Waiting for trigger
#define XGC_PROPAGATE 1  // Incremental mark (interruptible)
#define XGC_ATOMIC 2     // Atomic phase (non-interruptible)
#define XGC_SWEEP 3      // Sweep (non-incremental, includes inline finalization)
static inline const char *xr_gc_state_name(uint8_t state) {
    switch (state) {
        case XGC_PAUSE:
            return "PAUSE";
        case XGC_PROPAGATE:
            return "PROPAGATE";
        case XGC_ATOMIC:
            return "ATOMIC";
        case XGC_SWEEP:
            return "SWEEP";
        default:
            return "UNKNOWN";
    }
}

/* ========== Color Bit Definitions () ========== */

#define XGC_WHITE0 (1 << 0)  // White color 0
#define XGC_WHITE1 (1 << 1)  // White color 1
#define XGC_BLACK (1 << 2)   // Black (marked, refs scanned)
#define XGC_WHITEBITS (XGC_WHITE0 | XGC_WHITE1)
#define XGC_COLORBITS (XGC_WHITEBITS | XGC_BLACK)

// Color check macros
#define xr_gc_iswhite(o) (((o)->marked) & XGC_WHITEBITS)
#define xr_gc_isblack(o) (((o)->marked) & XGC_BLACK)
#define xr_gc_isgray(o) (((o)->marked & XGC_COLORBITS) == 0)

// Color set macros
#define xr_gc_white2gray(o) ((o)->marked &= ~XGC_WHITEBITS)
#define xr_gc_gray2black(o) ((o)->marked |= XGC_BLACK)
#define xr_gc_black2gray(o) ((o)->marked &= ~XGC_BLACK)
#define xr_gc_makewhite(gc, o)                                                                     \
    ((o)->marked = ((o)->marked & ~XGC_COLORBITS) | ((gc)->currentwhite & XGC_WHITEBITS))
#define xr_gc_set2black(o) ((o)->marked = ((o)->marked & ~XGC_WHITEBITS) | XGC_BLACK)

/* ========== Sticky Immix: Block-Level Generational GC ========== */
/*
 * Sticky Immix replaces per-object age with per-block age.
 * Objects in young blocks are young; objects in old blocks are old.
 *
 * Write barrier: when an old-block object writes a young-block child,
 * the parent is added to grayagain (remembered set).
 *
 * Remembered set tracking (bits 3-4 of marked, reusing old age bits):
 *   00 = not in remembered set
 *   01 = REMEMBERED1: first minor GC with this object in remembered set
 *   10 = REMEMBERED2: second (last) minor GC, will be removed after this
 *   barrierback sets to REMEMBERED1, each minor GC increments by 1.
 *   After 2 cycles without a new write, object exits remembered set.
 */
#define XGC_REM_SHIFT 3
#define XGC_REM_MASK (3 << XGC_REM_SHIFT)  // bits 3-4
#define XGC_REM_NONE 0
#define XGC_REM_1 1  // first cycle in remembered set
#define XGC_REM_2 2  // second cycle, will be evicted next

#define xr_gc_get_rem(o) ((((o)->marked) >> XGC_REM_SHIFT) & 3)
#define xr_gc_set_rem(o, r)                                                                        \
    ((o)->marked = (uint8_t) (((o)->marked & ~XGC_REM_MASK) | ((r) << XGC_REM_SHIFT)))

// All GC bits (color + remembered)
#define XGC_GCBITS (XGC_COLORBITS | XGC_REM_MASK)

/* ========== GC Mode ========== */

#define XGC_MODE_INC 0  // Incremental mark-sweep (used as major GC)
#define XGC_MODE_GEN 1  // Generational (minor collections)

/* ========== Gray List (separate from gc_next to avoid allgc chain corruption) ========== */

#define XR_GRAYLIST_INIT_CAP 32

typedef struct XrGCGrayList {
    XrGCHeader **items;
    int count;
    int capacity;
    int peak;  // High-water mark since last shrink check
} XrGCGrayList;

static inline void xr_gclist_init(XrGCGrayList *list) {
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
    list->peak = 0;
}

static inline void xr_gclist_destroy(XrGCGrayList *list) {
    if (list->items) {
        xr_free(list->items);
        list->items = NULL;
    }
    list->count = 0;
    list->capacity = 0;
}

static inline void xr_gclist_push(XrGCGrayList *list, XrGCHeader *obj) {
    if (list->count >= list->capacity) {
        int newcap = list->capacity ? list->capacity * 2 : XR_GRAYLIST_INIT_CAP;
        XrGCHeader **newp = (XrGCHeader **) xr_realloc(list->items, newcap * sizeof(XrGCHeader *));
        if (!newp) {
            // OOM during GC: abort to avoid silent data loss
            fprintf(stderr, "[GC] fatal: gray list realloc failed (OOM)\n");
            abort();
        }
        list->items = newp;
        list->capacity = newcap;
    }
    list->items[list->count++] = obj;
    if (list->count > list->peak)
        list->peak = list->count;
}

static inline XrGCHeader *xr_gclist_pop(XrGCGrayList *list) {
    return list->count > 0 ? list->items[--list->count] : NULL;
}

static inline void xr_gclist_reset(XrGCGrayList *list) {
    list->count = 0;
}

// Absorb all items from src into dst. Resets src.
static inline void xr_gclist_absorb(XrGCGrayList *dst, XrGCGrayList *src) {
    if (src->count == 0)
        return;
    if (dst->count == 0) {
        // Fast path: just swap buffers
        XrGCGrayList tmp = *dst;
        *dst = *src;
        *src = tmp;
        src->count = 0;
        return;
    }
    // Ensure capacity
    int total = dst->count + src->count;
    if (total > dst->capacity) {
        int newcap = dst->capacity ? dst->capacity : XR_GRAYLIST_INIT_CAP;
        while (newcap < total)
            newcap *= 2;
        XrGCHeader **newp = (XrGCHeader **) xr_realloc(dst->items, newcap * sizeof(XrGCHeader *));
        if (!newp)
            return;
        dst->items = newp;
        dst->capacity = newcap;
    }
    memcpy(dst->items + dst->count, src->items, src->count * sizeof(XrGCHeader *));
    dst->count = total;
    src->count = 0;
}

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

typedef enum {
    XGC_SWEEP_FULL_BLOCKS = 0,     // Sweeping full_blocks list
    XGC_SWEEP_RECYCLE_BLOCKS = 1,  // Sweeping recycle_blocks list
    XGC_SWEEP_CURRENT_BLOCK = 2,   // Sweeping current_block (single)
    XGC_SWEEP_LARGE_OBJECTS = 3,   // Sweeping large object list
    XGC_SWEEP_RECLAIM = 4,         // Reclassify blocks (full/recycle/free)
    XGC_SWEEP_DONE = 5             // Sweep complete
} XrSweepPhase;

/* ========== Coroutine GC Structure (Immix Mark-Region) ========== */

typedef struct XrCoroGC {
    // === Cache line 0: Immix allocator hot path ===
    // cursor/limit/current_block are the first 3 fields of XrImmixHeap
    XrImmixHeap immix;

    // === Allocation & GC trigger (follows immix in cache line 1) ===
    int64_t GCdebt;           // Debt bytes (triggers GC when > 0)
    int64_t totalbytes;       // Total allocated bytes
    int32_t gc_requested;     // GC requested, trigger at next safe point (int32 for JIT)
    uint8_t gcstate;          // GC phase: PAUSE/PROPAGATE/ATOMIC/SWEEP
    uint8_t currentwhite;     // Current white bit (XGC_WHITE0 or XGC_WHITE1)
    uint8_t in_gc;            // Re-entry guard
    uint8_t gc_disabled;      // Disable counter
    uint8_t gc_mode;          // XGC_MODE_INC or XGC_MODE_GEN
    uint8_t _pad1;            // alignment
    uint64_t alloc_since_gc;  // Bytes allocated since last GC

    // === Mark/sweep working data ===
    XrGCGrayList gray;       // Gray list (pending scan)
    XrGCGrayList grayagain;  // Need re-scan (back barrier)

    // Block-level incremental sweep state (see XrSweepPhase)
    int sweep_phase;            // XGC_SWEEP_FULL_BLOCKS .. XGC_SWEEP_DONE
    XrImmixBlock *sweep_block;  // Next block to sweep in current phase

    // === Generational GC state (Sticky Immix) ===
    int64_t GCest;           // Estimate of live bytes after last major GC
    int64_t young_promoted;  // Bytes in blocks promoted young→old this cycle

    // === Less frequent data ===
    XrGCGrayList weak;          // Weak tables
    XrGCHeader *large_objects;  // All large objects (single list)
    int64_t large_bytes;        // Total bytes in large_objects (for stats/tuning)

    int64_t GCmarked;  // Bytes marked in last GC

    // GC tuning parameters
    int gc_pause;    // Pause multiplier (100 = collect when memory doubles)
    int gc_stepmul;  // Step multiplier (controls GC speed vs mutator)

    // Ownership
    struct XrCoroutine *owner;

    // External root callbacks (for C extensions holding GC objects)
    XrCoroGCRootEntry *root_callbacks;

    // Shared object refcount tracking (Erlang ProcBin equivalent)
    // During mark phase, shared objects encountered are recorded in shared_refs.
    // After sweep, objects in prev_shared_refs but not in shared_refs get decref'd.
    XrGCHeader **shared_refs;
    int shared_refs_count;
    int shared_refs_capacity;
    XrGCHeader **prev_shared_refs;
    int prev_shared_refs_count;

    // Statistics (cold)
    uint32_t gc_count;
    uint32_t object_count;       // Total live GC objects (incremental counter)
    uint64_t gc_time_ns;         // Cumulative GC time across all cycles
    uint64_t last_gc_time_ns;    // Duration of last completed GC cycle
    uint64_t gc_cycle_start_ns;  // Start time of current incremental cycle
    uint32_t finalizer_count;    // Total finalizers called

    // Per-phase timing (reset each cycle)
    uint64_t mark_time_ns;   // Time spent in PROPAGATE + ATOMIC
    uint64_t sweep_time_ns;  // Time spent in SWEEP
    uint64_t mark_start_ns;  // Timestamp when mark phase began (internal)

    // Per-cycle counters (reset each cycle)
    uint32_t objects_marked;     // Objects traversed during mark
    uint32_t objects_swept;      // Objects freed during sweep
    uint32_t objects_finalized;  // Objects with finalizers called
    uint32_t objects_promoted;   // Young blocks promoted to old (gen mode)

    // === RC per-object freelist (RC reclaims small objects) ===
    // Placed last so it does not shift any JIT-hardcoded field offset.
    // Segregated free lists by 16-byte size class, lazily allocated; on
    // drop-to-zero a small object's memory is pushed here and reused by a
    // later same-class allocation before falling back to Immix bump.
    // NULL until the first free in this coroutine (idle coros pay nothing).
    XrGCHeader **rc_freelist;  // array[XR_RC_FREECLASSES] of list heads
} XrCoroGC;

// Hot-path fields (immix + GCdebt + gc_requested) must be in first 2 cache lines
_Static_assert(offsetof(XrCoroGC, gc_requested) < 128,
               "gc_requested must be within first 2 cache lines for JIT fast path");
_Static_assert(offsetof(XrCoroGC, GCdebt) < 128, "GCdebt must be within first 2 cache lines");

/* ========== JIT Struct Offsets (compile-time constants) ========== */

#define XR_COROGC_OFFSET_IMMIX offsetof(XrCoroGC, immix)
#define XR_COROGC_OFFSET_GCSTATE offsetof(XrCoroGC, gcstate)
#define XR_COROGC_OFFSET_CURRENTWHITE offsetof(XrCoroGC, currentwhite)
#define XR_COROGC_OFFSET_GC_REQUESTED offsetof(XrCoroGC, gc_requested)
#define XR_COROGC_OFFSET_GCDEBT offsetof(XrCoroGC, GCdebt)
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
    if (gc->gc_disabled == 0) {
        gc->GCdebt += bytes;
        if (gc->GCdebt > 0 && !gc->in_gc)
            gc->gc_requested = 1;
    }
}

static inline void xr_gc_sub_external(XrCoroGC *gc, int64_t bytes) {
    if (!gc)
        return;
    gc->totalbytes -= bytes;
    gc->GCdebt -= bytes;
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
