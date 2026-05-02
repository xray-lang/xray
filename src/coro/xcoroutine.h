/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcoroutine.h - Coroutine type definitions
 *
 * KEY CONCEPT:
 *   - XrCoroutine: unified coroutine object (VM execution unit + user-visible task)
 *   - Per-coroutine VM execution context, value stack, and bytecode frames
 *   - 3-level priority scheduling (LOW/NORMAL/HIGH)
 *   - Reduction-based fair scheduling
 *
 * SCHEDULING INVARIANTS:
 *
 *   INVARIANT 1 (State exclusivity): A coroutine is in exactly ONE state
 *   at any time: READY, RUNNING, BLOCKED, or DONE. The state is encoded
 *   in atomic flags. Transitions are CAS-guarded to prevent races.
 *
 *   INVARIANT 2 (Single runner): At most one Worker executes a coroutine
 *   at any time. A coroutine moves from a run queue to RUNNING only on
 *   the Worker that dequeues it. No other Worker may touch its stack.
 *
 *   INVARIANT 3 (Queue membership): A READY coroutine is on exactly ONE
 *   queue: a Worker's local run queue, LIFO slot, or MPSC inbox.
 *   A BLOCKED coroutine is on a channel wait queue or timer wheel.
 *   RUNNING and DONE coroutines are on no queue.
 *
 *   INVARIANT 4 (Reduction fairness): Each coroutine starts a time slice
 *   with XR_CORO_REDUCTIONS. The VM decrements on backward jumps and
 *   calls. When reductions <= 0, the coroutine yields to the scheduler.
 *   This prevents any single coroutine from starving others.
 *
 *   INVARIANT 5 (Affinity hint): affinity_p is a hint for wake targeting.
 *   It is NOT a hard binding. The scheduler may migrate coroutines via
 *   work-stealing. Only channel_wake_coro uses affinity_p to choose
 *   the target Worker for waking a blocked coroutine.
 *
 *   INVARIANT 6 (GC isolation): Each coroutine has its own GC heap
 *   (XrCoroGC). Cross-coroutine object transfer requires deep copy
 *   or shared storage (reference counted). A coroutine's GC never
 *   touches another coroutine's heap objects.
 *
 * COROUTINE STATE MACHINE:
 *
 *   READY ──► RUNNING ──► READY     (yield / preempt)
 *     │          │
 *     │          ├──► BLOCKED        (channel send/recv, sleep, I/O)
 *     │          │       │
 *     │          │       └──► READY  (wake: channel data, timer, I/O ready)
 *     │          │
 *     │          └──► DONE           (function returns / unhandled exception)
 *     │
 *     └── created via go statement, starts as READY
 */

#ifndef XCOROUTINE_H
#define XCOROUTINE_H

#include <stdatomic.h>
#include "../base/xconstants.h"
#include "xexec_frame.h"  // XrBcCallFrame, XrClosure, XrValue
#include "xcoro_flags.h"
#include "xtimer_wheel.h"

/* ========== Forward Declarations ========== */

struct XrCoroGC;
struct XrCoroMonitor;
struct XrCoroRegistry;

/* ========== Coroutine Priority ========== */

typedef enum {
    CORO_PRIORITY_LOW = 0,
    CORO_PRIORITY_NORMAL = 1,
    CORO_PRIORITY_HIGH = 2
} XrCoroPriority;

#define XR_CORO_PRIORITY_COUNT 3

#define XR_GC_FLG_NEED_GC 0x0001
#define XR_GC_FLG_IN_GC 0x0002
#define XR_GC_FLG_PROMOTED 0x0004

/* ========== Coroutine Entry Type ========== */

typedef enum {
    XR_CORO_ENTRY_CLOSURE,
    XR_CORO_ENTRY_NATIVE,
    XR_CORO_ENTRY_CFUNC
} XrCoroEntryType;

typedef union {
    XrClosure *closure;
    struct {
        void (*func)(void *);
        void *arg;
    } native;
    XrCFuncResult (*cfunc)(struct XrayIsolate *, XrValue *, int, XrValue *);
} XrCoroEntry;

/* ========== JIT Scratch Space (per-Worker, not per-coroutine) ==========
 *
 * JIT functions are synchronous — they never yield (channel/spawn/sleep
 * cause deopt back to interpreter). So only one JIT execution is active
 * per worker at any time. This scratch space lives in XrProc (per-Worker P).
 *
 * Previously these fields were embedded in XrCoroutine (~784B per coro).
 * Moving them to per-Worker saves ~776B per coroutine.
 */
typedef struct XrJitScratch {
    int64_t call_args[16];      // Raw unboxed arguments (max 15 params + closure)
    uint8_t call_arg_tags[16];  // Compile-time XR_TAG_* for each call_arg (set by codegen)
    void *call_proto;           // Current proto for CALLSELF (XrProto*)
    void *call_closure;         // Current closure for upvalue access (XrClosure*)
    void *exception;            // Non-NULL when exception pending in JIT code
    int64_t ret_count;  // Number of return values (0 = single via x0); int64_t for 8-byte alignment
    uint32_t deopt_id;  // Deopt point ID (set by deopt stub)
    uint32_t invoke_deopt_id;  // Valid deopt_id for CALL_C invoke recovery (deopt_id=0 safe)

    /* Param tags: runtime XrValue.tag for each argument, set by xm_jit_call.
     * Used by JIT null-check codegen to distinguish int(0) from null
     * for nullable primitive params (int?/float?/bool?). */
    int64_t param_tags[8];

    /* Multi-return values: ret_vals[0] = 2nd return value, ret_vals[1] = 3rd, etc.
     * First return value goes through x0 as usual.
     * Tags for reconstruction stored in ret_tags[]. */
    int64_t ret_vals[7];  // Extra return values (max 8 total, 1st in x0)
    int64_t ret_tags[7];  // XrValue tags for ret_vals[] (int64_t for 8-byte alignment)

    /* Deopt register snapshot: saved by deopt stub before returning DEOPT_MARKER.
     * Indexed by physical register number for O(1) lookup.
     * GP: x0-x28 (29 slots), FP: d0-d15 (16 slots) */
    int64_t deopt_regs[29];
    int64_t deopt_fp_regs[16];
    int64_t deopt_spill_base;  // Frame pointer at deopt (legacy, kept for GC)

    /* Spill slot snapshot: copied from frame by deopt stub BEFORE epilogue.
     * Indexed by spill slot number: deopt_spill_save[slot] = frame[SPILL_BASE + slot*8].
     * Recovery reads from here instead of the frame (which is deallocated after epilogue).
     * Max slots = XM_MAX_SPILL_SLOTS (32). */
    int64_t deopt_spill_save[32];

    int32_t osr_deopt_pc;  // OSR deopt recovery: bytecode PC to resume (-1 = none)

    /* GC stack map: compile-time bitmap for precise GC root scanning.
     * active_safepoint_id indexes into active_stack_map->entries[] to find
     * which registers/spill slots hold GC pointers at the current safepoint. */
    uint32_t active_safepoint_id;  // current safepoint index (UINT32_MAX = none)
    void *active_stack_map;        // XrStackMapTable* for current JIT function
    void *jit_frame_sp;            // FP of current (innermost) JIT frame
    void *safepoint_saved_sp;      // SP saved by safepoint stub (for reading saved regs)

    /* JIT frame stack: caller FPs pushed before cross-function JIT calls.
     *
     * WHY THIS DESIGN:
     *   - Replaces gc_root_chain linked list with simple array
     *   - Each cross-function call pushes caller FP before call, pops after
     *   - GC walks frame_stack to scan caller frames' spill slots
     *   - Caller writes PTR reg values back to spill slots before push
     *   - Max depth 16 covers all practical JIT call nesting
     *   - O(1) push/pop vs linked list traversal
     */
#define XR_JIT_MAX_FRAME_DEPTH 16
    uint32_t jit_frame_depth;
    void *jit_frame_stack[XR_JIT_MAX_FRAME_DEPTH];

    /* Per-slot runtime tags: written by CALL_C codegen after each CALL_C.
     * Indexed by bytecode register number (bc_slot).
     * Consumers that need a dynamic tag (INDEX_SET, PRINT, TYPEOF, ISNULL, etc.)
     * read slot_runtime_tags[bc_slot] instead of the old global return_tag. */
    uint8_t slot_runtime_tags[256];

    /* Tag returned by the last call_c_stub invocation.
     * call_c_stub stores the C helper's x1 here instead of returning it in
     * x1, so that x1 (alloc_regs[0]) is not clobbered by the stub return
     * sequence.  XM_CALL_C codegen reads this field to populate
     * slot_runtime_tags[bc_slot]. */
    int64_t call_result_tag;

    /* JIT yield: pre-push frame for yieldable suspend.
     * When xr_jit_invoke_method detects WOULD_BLOCK (try-mode), it reads
     * register values from saved areas (safepoint_saved_sp + jit_frame_sp),
     * populates the VM stack, pre-pushes an interpreter frame, and calls
     * the yieldable in normal mode. If BLOCKED/YIELD, these fields signal
     * the VM to skip deopt recovery and return the appropriate result. */
    int32_t call_base_offset;  // callee base_offset (set by VM before xm_jit_call)
    bool yield_frame_pushed;   // helper pre-pushed frame, yieldable blocked/yielded
    uint8_t yield_vm_result;   // XR_VM_BLOCKED or XR_VM_YIELD

    /* Heartbeat pointer: set by run_on_worker to &machine->heartbeat.
     * Bumped by xr_coro_gc_safepoint so sysmon doesn't misdetect
     * long-running JIT code as stuck (JIT doesn't return to worker
     * loop between C helper calls like spawn_cont/await). */
    _Atomic uint64_t *heartbeat_ptr;

    /* Guard page safepoint: x20 (SAFEPT_PAGE_REG) points here.
     * Normal: PROT_READ, ldr wzr,[x20] succeeds (zero overhead).
     * Armed:  PROT_NONE, ldr faults → SIGSEGV → trampoline → safepoint.
     * Sysmon periodically arms via mprotect; trampoline disarms after work. */
    void *safepoint_page;       // mmap'd guard page (one per worker)
    void *safepoint_return_pc;  // saved PC+4 after guard page fault

} XrJitScratch;

/* ========== Await State Machine ========== */

typedef enum {
    XR_AWAIT_NONE = 0,      // not awaited (initial / consumed)
    XR_AWAIT_WAITING = 1,   // parent suspended, waiting for child
    XR_AWAIT_RESOLVED = 2,  // child completed, result in coro->result
} XrAwaitState;

/* ========== XrCoroExt - Cold fields allocated on demand ========== */

typedef struct XrCoroExt {
    char *io_buf;  // I/O read buffer (reused across calls)
    size_t io_buf_cap;
    struct XrMap *locals;              // Per-coroutine dynamic locals (debug/inspect)
    struct XrCoroMonitor *watched_by;  // Monitor list head (lifecycle watchers)

    /* === I/O yield state (only set during Yieldable I/O or sleep) === */
    struct {
        int wait_fd;        // fd being waited on (-1 = no fd)
        int wait_events;    // requested events (POLLIN/POLLOUT)
        int result_events;  // events returned by netpoll
        int64_t deadline;   // absolute deadline in microseconds (-1 = none)
        bool timed_out;
    } yield_info;

    /* === Thread-lock extras (only set when Coro.lockThread() is called) === */
    _Atomic int lock_count;  // lock nesting depth (0 = unlocked)
    int locked_worker;       // Worker ID that owns the lock (-1 = none)

    /* === Timer (only allocated on first sleep/timeout use) === */
    XrTWheelTimer timer;
    _Atomic bool timer_active;
    int timer_wheel_owner;  // Worker ID that owns the timer (-1 = none)
    _Atomic uintptr_t timer_seq;
} XrCoroExt;

/* ========== XrCoroutine - Coroutine Object ========== */

/*
 * Capacity of XrJitSuspendState::spill[].
 *
 * Single source of truth consumed by JIT codegen and LSRA eligibility:
 *   - codegen stores/restores at most this many spill slots across a
 *     SUSPEND/RESUME transition;
 *   - a function containing an AWAIT/SUSPEND that requires more spill
 *     slots than this must be refused JIT compilation (otherwise the
 *     extra slots would be lost across the suspend bridge).
 *
 * If this value is raised, the _Static_assert in xm_offsets.h that checks
 * XM_SUSPEND_SPILL_OFF also needs to be revisited because sizeof the
 * containing struct changes.
 */
#define XM_SUSPEND_SPILL_MAX 15

/*
 * JIT suspend state: saved registers across suspend/resume.
 * Heap-allocated on demand (lazy) to save 320 bytes per non-JIT coroutine.
 *
 * MEMORY LAYOUT (320 bytes = 40 * int64_t):
 *   +0    caller_saved[15]  x1-x15  (scratch regs, saved by XM_SUSPEND)
 *   +120  callee_saved[8]   x20-x27 (callee-saved, for cross-worker resume)
 *   +184  result            await/channel return value slot
 *   +192  result_tag        XR_TAG_* for result (written alongside result by waker)
 *   +200  spill[XM_SUSPEND_SPILL_MAX] spill slots bridging old→new stack frame
 */
typedef struct XrJitSuspendState {
    int64_t caller_saved[15];  // x1-x15
    int64_t callee_saved[8];   // x20-x27
    int64_t result;            // await/channel result (written by block helper or waker)
    int64_t result_tag;        // XR_TAG_* for result (resume writes to runtime_tags)
    int64_t spill[XM_SUSPEND_SPILL_MAX];  // spill slots (old frame → suspend → new frame)
} XrJitSuspendState;

typedef struct XrCoroutine {
    /* ================================================================
     * HOT ZONE (first 64 bytes) — accessed every schedule/yield cycle
     * ================================================================ */
    XrGCHeader gc;                   // 16 bytes: GC header (must be first)
    _Atomic uint32_t flags;          //  4 bytes: state flags (every dispatch)
    int32_t reductions;              //  4 bytes: remaining before yield (JIT: check <= 0)
    struct XrCoroutine *sched_link;  //  8 bytes: MPSC/steal queue linkage
    struct XrCoroutine *next;        //  8 bytes: blocked/ready list linkage
    struct XrCoroutine *prev;        //  8 bytes: blocked/ready list linkage
    _Atomic int resume_status;       //  4 bytes: checked on every resume
    _Atomic int affinity_p;          //  4 bytes: preferred worker for wake (relaxed ok, hint only)
    int id;                          //  4 bytes: coroutine ID
    int8_t schedule_count;           //  1 byte: schedule counter (max XR_RESCHEDULE_LOW=8)
    _Atomic(uint8_t) coro_state;  //  1 byte: authoritative state (R4: replaces state bits in flags)
    uint16_t gc_flags;            //  2 bytes: GC flags (bit 0: VM stack slab)
    // --- 64 bytes boundary ---

    /* === Work Stealing Freshness (set on enqueue, read on steal peek) === */
    int64_t submit_time;  //  8 bytes: monotonic ms when enqueued to run queue

    /* === VM Execution Context === */
    XrVMContext vm_ctx;

    /* ================================================================
     * WARM ZONE (Cache Line 3, offset 192+) — JIT/GC/result hot fields
     * Grouped here to minimize cache misses on JIT entry and GC safepoint.
     * ================================================================ */
    struct XrJitScratch *jit_ctx;  // JIT prologue loads x28 from this
    struct XrCoroGC *coro_gc;      // GC safepoint: checked every loop back-edge
    struct XrayIsolate *isolate;   // JIT runtime helpers use 22+ times
    XrValue result;
    XrValue error;
    XrValue pending_closure_result;  // return value from xr_yield_call_closure

    /* === Task Handle (GC-managed user-visible handle) === */
    struct XrTask *task;  // back-pointer to associated XrTask (NULL for main coro)

    /* === Await/Wait Support (caller-side only; awaited-side fields live on XrTask) === */
    struct XrTask *_Atomic await_task;  // task being awaited (for post-check race detection)
    _Atomic int wait_count;
    _Atomic bool any_done;
    struct XrArray *await_results;
    struct XrScopeContext *parent_scope;
    struct XrCoroutine *scope_sibling;  // linked list within parent_scope

    /* === Channel Blocking === */
    struct XrCoroutine *wait_link;  // channel waitq linkage (separate from sched_link)
    void *wait_channel;
    bool wait_send;
    XrValue send_value;
    XrValue *recv_slot;
    int recv_slot_offset;
    int64_t channel_deadline;
    struct XrSelectWait *select_wait;
    int select_ready_case;

    /* === Resume State (extended) === */
    int16_t pending_result_slot;
    bool jit_try_mode;  // JIT try-mode: yieldable should return WOULD_BLOCK instead of blocking

    /* === JIT Suspend/Resume ===
     * When JIT code hits AWAIT and the child isn't done, it saves live
     * registers here and returns XM_SUSPEND_MARKER. On resume, the
     * worker calls jit_resume_entry which reloads registers and jumps
     * to the continuation point via jit_suspend_id. */
    void *jit_resume_entry;        // resume code address (NULL = not JIT-suspended)
    void *jit_resume_proto;        // XrProto* that owns the resume code (for stack map)
    uint32_t jit_suspend_id;       // resume point index (jump table dispatch)
    uint32_t jit_suspend_smap_id;  // stack map id at suspend point (for GC)

    /*
     * JIT suspend state: saved registers across suspend/resume.
     * Allocated on demand (lazy) to save 320 bytes per non-JIT coroutine.
     * NULL until the first JIT call/resume that needs suspend support.
     */
    XrJitSuspendState *jit_suspend;

    /* === Identity (set once at creation, cold path) === */
    const char *name;
    const char *source_file;
    int source_line;

    /* === Entry Point (set once at creation) === */
    XrCoroEntryType entry_type;
    XrCoroEntry entry;
    XrValue *args;
    int arg_count;
    XrValue inline_args[4];

    /* === Continuation Stealing === */
    struct XrCoroutine *pending_spawn;

    /* === Per-Coroutine Scope Tracking === */
    struct XrScopeContext *current_scope;

    /* === Async Stack Trace (lazy capture) === */
    struct XrCoroutine *parent_coro;
    int spawn_line;
    const char *spawn_file;

    /* === Cold Extension (io_buf, locals, watched_by — allocated on demand) === */
    XrCoroExt *ext;
} XrCoroutine;

/* ========== XrCoroExt Accessor ========== */

#include "../base/xmalloc.h"

static inline XrCoroExt *xr_coro_ensure_ext(XrCoroutine *coro) {
    if (!coro->ext) {
        coro->ext = (XrCoroExt *) xr_calloc(1, sizeof(XrCoroExt));
    }
    return coro->ext;
}

/* ========== Thread-Lock Query Helpers ========== */

// Check if coroutine is pinned to a specific worker via Coro.lockThread().
static inline bool xr_coro_is_thread_locked(XrCoroutine *coro) {
    if (!coro->ext)
        return false;
    return atomic_load_explicit(&coro->ext->lock_count, memory_order_relaxed) > 0 &&
           coro->ext->locked_worker >= 0;
}

// Effective wake target: locked_worker if thread-locked, else affinity_p.
// All wake-routing paths should use this instead of reading affinity_p directly,
// so that Coro.lockThread() is respected across channel wake, sleep timeout,
// async completion, netpoll, and scope wake.
static inline int xr_coro_wake_target_id(XrCoroutine *coro) {
    if (coro->ext) {
        int lc = atomic_load_explicit(&coro->ext->lock_count, memory_order_relaxed);
        if (lc > 0 && coro->ext->locked_worker >= 0) {
            return coro->ext->locked_worker;
        }
    }
    return atomic_load_explicit(&coro->affinity_p, memory_order_relaxed);
}

/* ========== JIT Integration APIs ========== */

// Check if coroutine should yield (for JIT loop back-edges)
// JIT only needs: load coro->reductions; cmp 0; jle yield_stub
static inline bool xr_coro_should_yield(XrCoroutine *coro) {
    return coro->reductions <= 0;
}

// Request yield at next safepoint (preempt, GC, cancel)
// Forces reductions to 0 so the single-check safepoint triggers
static inline void xr_coro_request_yield(XrCoroutine *coro) {
    coro->reductions = 0;
}

// GC safepoint for JIT code: GC step + cancel check.
// Returns 0 to continue, non-zero to request deopt exit.
// Backend stub contract: check return value, jump to deopt_stub if non-zero.
XR_FUNC int xr_coro_gc_safepoint(XrCoroutine *coro);

// JIT write barriers: thin wrappers around GC barrier functions
XR_FUNC void xr_jit_barrier_fwd(XrCoroutine *coro, void *parent, void *child);
XR_FUNC void xr_jit_barrier_back(XrCoroutine *coro, void *container);

/* ========== Helper Structures ========== */

typedef struct XrSelectCase {
    void *channel;
    bool is_send;
    XrValue send_value;
    int result_reg;
    // Per-bucket select queue linkage (avoids O(N) blocked scan).
    struct XrSelectCase *bucket_next;  // next case node in same bucket's select queue
    XrCoroutine *owner;                // back-pointer to owning coroutine
} XrSelectCase;

typedef struct XrSelectWait {
    XrSelectCase *cases;
    int case_count;
    void *timer_channel;
    int timer_case_index;
    _Atomic bool triggered;
} XrSelectWait;

typedef struct XrBlockedBucket {
    void *channel;
    XrCoroutine *send_head;
    XrCoroutine *send_tail;
    XrCoroutine *recv_head;
    XrCoroutine *recv_tail;
    XrSelectCase *select_head;  // Per-channel select case chain.
    XrSelectCase *select_tail;
    struct XrBlockedBucket *next;
} XrBlockedBucket;

/* ========== Memory Sync Helpers ========== */

XR_FUNC void xr_coro_sync_vm_ctx(XrCoroutine *coro, struct XrayIsolate *X);
XR_FUNC bool xr_coro_upgrade_heap(XrCoroutine *coro, size_t size);

/* ========== Bootstrap Main Coroutine ========== */

// Create minimal coro during isolate init (before any script execution)
XR_FUNC XrCoroutine *xr_coro_create_bootstrap(struct XrayIsolate *X);
// Upgrade bootstrap coro with closure for script execution
XR_FUNC void xr_coro_setup_main(XrCoroutine *coro, struct XrayIsolate *X, XrClosure *closure);
// Reset main_coro for sequential re-execution (test runner, REPL)
// Resets vm_ctx, result/error fields, and sets new closure.
// Caller then calls xr_main_thread_run() which handles flag reset.
XR_FUNC void xr_coro_reset_for_call(XrCoroutine *coro, struct XrayIsolate *X, XrClosure *closure);

// Native-stackful coroutine creation was removed; use xr_coro_create_cfunc instead.

/* ========== VM Stack Growth ========== */

// Grow coroutine VM stack and/or frame array.
// Pure stack management — no GC interaction.
XR_FUNC bool xr_coro_grow_stack(XrCoroutine *coro, int extra_slots);

/* ========== Scope Context ==========
 *
 * XrScopeContext represents a single `scope { ... }` block at runtime
 * and is ORTHOGONAL to the XrTask tree (xtask.h). They look similar
 * because both express "parent / child" relationships, but they
 * answer different questions and are NOT redundant — merging them
 * would lose one of the two semantic dimensions:
 *
 *   XrTask tree (xtask.h)
 *     - one node per `go` expression
 *     - parent / child links describe "who awaits whom"
 *     - GC-managed (~112B), survives executor recycle
 *     - lives across the whole task lifecycle
 *
 *   XrScopeContext (this struct)
 *     - one node per `scope { ... }` / `linked scope { ... }` /
 *       `supervisor scope { ... }` block
 *     - first_child links describe "which coros run inside this block"
 *     - malloc-allocated, freed at OP_SCOPE_EXIT
 *     - carries the per-block POLICY (mode, first_error, errors[],
 *       cancel_requested) that does not exist on XrTask
 *
 * A single coroutine has both a parent_task (for await) and a
 * parent_scope (for the structured-concurrency wait barrier). Each
 * scope block can contain multiple tasks; a top-level `go fn()`
 * outside any scope has no parent_scope at all.
 *
 * Concurrency: child_lock serializes mutations of first_child,
 * first_error, errors[], and cancel_requested under the dispatcher
 * in xcoro.c (xr_coro_wake_waiter); see the wake-waiter sub-step
 * doc comments there for the full lock contract. errors[] is
 * preallocated at OP_SCOPE_ENTER for the supervisor mode so the
 * locked section never needs to allocate. */

typedef struct XrScopeContext {
    _Atomic int count;
    struct XrScopeContext *parent;
    uint8_t mode;                     // XrScopeMode
    _Atomic bool cancel_requested;    // linked scope: set when first child fails
    _Atomic bool child_lock;          // Spinlock — see lock contract above
    XrValue first_error;              // linked scope: first child error (lock-protected)
    struct XrArray *errors;           // supervisor scope: collected errors (eager-alloc)
    struct XrCoroutine *first_child;  // linked list of child coros in this scope
} XrScopeContext;

/* ========== Coroutine State (single-thread scheduler + isolate-level coro bookkeeping) ==========
 */

typedef struct XrCoroState {
    XrCoroutine *ready_head[XR_CORO_PRIORITY_COUNT];
    XrCoroutine *ready_tail[XR_CORO_PRIORITY_COUNT];
    int coro_count;
    _Atomic int total_created;
    XrScopeContext *current_scope;
    struct XrCoroRegistry *coro_registry;  // Named coroutine registry (lazy init)
} XrCoroState;

/* ========== Coroutine API ========== */

struct XrayIsolate;
struct XrClosure;

// Lifecycle
XR_FUNC XrCoroutine *xr_coro_create(struct XrayIsolate *X, struct XrClosure *closure, XrValue *args,
                                    int arg_count, const char *name, const char *file, int line);
XR_FUNC void xr_coro_free(XrCoroutine *coro);
XR_FUNC void xr_coro_release_heap(XrCoroutine *coro);
XR_FUNC void xr_coro_release_resources(XrCoroutine *coro);
XR_FUNC void xr_coro_spawn(struct XrayIsolate *X, XrCoroutine *coro);

// Scheduler
XR_FUNC void xr_sched_init(XrCoroState *sched);
XR_FUNC void xr_sched_destroy(XrCoroState *sched);
XR_FUNC void xr_sched_enqueue(XrCoroState *sched, XrCoroutine *coro);
XR_FUNC void xr_sched_remove(XrCoroState *sched, XrCoroutine *target);
XR_FUNC XrCoroutine *xr_sched_dequeue(XrCoroState *sched);

// Multicore runtime
XR_FUNC void xr_multicore_init(struct XrayIsolate *X, int num_workers);
XR_FUNC void xr_multicore_destroy(struct XrayIsolate *X);

// Wake mechanism
XR_FUNC void xr_coro_ready(struct XrayIsolate *X, XrCoroutine *gp, bool next);
XR_FUNC XrCoroutine *xr_current_coro(struct XrayIsolate *X);
XR_FUNC void xr_coro_wake_waiter(struct XrayIsolate *X, XrCoroutine *coro);

// Channel wake (auto fallback to single-thread mode)
XR_FUNC XrCoroutine *xr_runtime_wake_channel(struct XrayIsolate *X, void *channel,
                                             bool wake_sender);
XR_FUNC void xr_runtime_wake_channel_all(struct XrayIsolate *X, void *channel);

// Control
XR_FUNC void xr_coro_cancel(XrCoroutine *coro);

// Scope structured concurrency
XR_FUNC void xr_scope_add_coro(XrCoroState *sched, XrCoroutine *coro, XrCoroutine *parent);

#endif  // XCOROUTINE_H
