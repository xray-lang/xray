/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmachine.h - Machine (M/OS thread) definitions for P/M split scheduler
 *
 * KEY CONCEPT:
 *   XrMachine is an OS thread abstraction (analogous to Go's M).
 *   M count is dynamic: starts at N (= P count), grows on demand.
 *   An M must acquire a P (XrProc) before it can schedule coroutines.
 *   When M blocks in C code, it releases P so another M can use it.
 *
 * WHY THIS DESIGN:
 *   - Decouples OS threads from scheduling resources
 *   - Blocked C functions don't waste scheduling slots
 *   - Thread count grows only when needed (handoff creates new M)
 *
 * RELATED MODULES:
 *   - xproc.h: Scheduling resource (P) that M acquires
 *   - xworker.h: Worker = P + M* combined
 */

#ifndef XMACHINE_H
#define XMACHINE_H

#include "../os/os_thread.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "xexec_frame.h"
#include "../runtime/xvm_call.h"
#include "../base/xplatform.h"

/* ========== Futex-based Park/Unpark ========== */

#define XR_PARK_IDLE 0      // Parked, waiting for wake
#define XR_PARK_WOKEN 1     // Woken by signal
#define XR_PARK_NOTIFIED 2  // Pre-notification before park

#ifdef XR_OS_LINUX
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
static inline void xr_park_futex_wait(_Atomic int *addr, int expected, uint32_t timeout_us) {
    if (timeout_us == 0) {
        syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0);
    } else {
        struct timespec ts = {.tv_sec = timeout_us / 1000000,
                              .tv_nsec = (timeout_us % 1000000) * 1000};
        syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, &ts, NULL, 0);
    }
}
static inline void xr_park_futex_wake(_Atomic int *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}
#elif defined(XR_OS_MACOS)
#include <errno.h>
#include <time.h>
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define XR_UL_COMPARE_AND_WAIT_M 1
static inline void xr_park_futex_wait(_Atomic int *addr, int expected, uint32_t timeout_us) {
    __ulock_wait(XR_UL_COMPARE_AND_WAIT_M, addr, (uint64_t) (uint32_t) expected, timeout_us);
}
static inline void xr_park_futex_wake(_Atomic int *addr) {
    __ulock_wake(XR_UL_COMPARE_AND_WAIT_M, addr, 0);
}
#elif defined(XR_OS_WINDOWS)
#include <windows.h>
static inline void xr_park_futex_wait(_Atomic int *addr, int expected, uint32_t timeout_us) {
    DWORD ms = (timeout_us == 0) ? INFINITE : (timeout_us / 1000);
    WaitOnAddress(addr, &expected, sizeof(int), ms);
}
static inline void xr_park_futex_wake(_Atomic int *addr) {
    WakeByAddressSingle(addr);
}
#endif  // Forward declarations
struct XrProc;
struct XrRuntime;
struct XrCoroutine;
struct XrWorker;

/* ========== M State ========== */

typedef enum {
    M_IDLE,      // Woke from park, no coroutine yet
    M_RUNNING,   // Actively executing a coroutine
    M_SPINNING,  // Looking for work (spinning)
    M_STEALING,  // Stealing from another P's run queue
    M_PARKING,   // Transitioning to parked (entering sleep)
    M_PARKED,    // Sleeping, waiting to be woken
    M_BLOCKED,   // Blocked in C code / syscall (no P)
    M_SHUTDOWN   // Shutting down
} XrMachineState;

/* ========== Machine VM Storage ========== */

#define XR_MACHINE_STACK_SIZE 1024
#define XR_MACHINE_FRAME_SIZE 64
#define XMACHINE_HANDLER_SIZE 16

typedef struct XrMachineVMStorage {
    XrValue stack[XR_MACHINE_STACK_SIZE];
    XrBcCallFrame frames[XR_MACHINE_FRAME_SIZE];
    XrExceptionHandler handlers[XMACHINE_HANDLER_SIZE];
} XrMachineVMStorage;

/* ========== XrMachine (M) — OS thread, dynamic count ========== */

typedef struct XrMachine {
    /* === Thread Identity === */
    xr_thread_t thread;
    int id;

    /* === Current P (NULL = idle/blocked) === */
    _Atomic(struct XrProc *) current_p;

    /* === Thread-local VM Context === */
    XrVMContext vm_ctx;
    XrMachineVMStorage vm_storage;
    struct XrCoroutine *current_coro;

    /* === State === */
    _Atomic int state;  // XrMachineState
    bool spinning;

    /* === Sleep/Wake Mechanism (futex-based, replaces pthread mutex+cond) === */
    _Atomic int park_state;  // XR_PARK_IDLE / XR_PARK_WOKEN / XR_PARK_NOTIFIED

    /* === Heartbeat (sysmon uses) === */
    _Atomic uint64_t heartbeat;

    /* === Current C function (for sysmon auto-upgrade) === */
    void *current_cfunc;  // XrCFunction* when executing C code, NULL otherwise

    /* === Next P to acquire (set before unpark) === */
    struct XrProc *next_p;

    /* === Syscall handoff: Worker that M was running when it blocked === */
    struct XrWorker *blocked_worker;

    /* === M Linked Lists === */
    struct XrMachine *all_link;   // Global all-M list
    struct XrMachine *idle_link;  // Idle list link (shared by idle_worker_list
                                  // OR idle_m_head at any moment — see xworker.h)

    /* === Idle-stack guard ===
     * Prevents the same M from being pushed twice onto idle_worker_list.
     * Set to true by idle_worker_push CAS, cleared by idle_worker_pop.
     * This is required because we removed idle_worker_remove (a lock-free
     * O(1) mid-list removal is not implementable without hazard pointers),
     * so a self-woken M stays in the list until a subsequent wake_idle_worker
     * pops it. Without this flag, the M would be pushed a second time at its
     * next park and form a cycle. */
    _Atomic bool in_idle_worker_list;

    /* === Runtime back pointer === */
    struct XrRuntime *runtime;

    /* === Thread Reuse (handoff M keeps thread alive) === */
    _Atomic bool has_thread;  // true if M has a parked thread waiting for next_p
} XrMachine;

/* ========== M Lifecycle API ========== */

// Allocate and initialize a new M
XR_FUNC XrMachine *xr_machine_alloc(struct XrRuntime *runtime, int id);

// Initialize M fields (for stack-allocated or pre-allocated M)
XR_FUNC void xr_machine_init(XrMachine *m, int id, struct XrRuntime *runtime);

// Destroy M resources
XR_FUNC void xr_machine_destroy(XrMachine *m);

// Park M (block until woken)
// Reference: Go src/runtime/proc.go stopm()
XR_FUNC void xr_park_m(XrMachine *m);

// Unpark M (wake from sleep)
XR_FUNC void xr_unpark_m(XrMachine *m);

/* ========== Idle M Management ========== */

// Get an idle M from runtime pool
XR_FUNC XrMachine *xr_get_idle_m(struct XrRuntime *runtime);

// Put M into idle pool
XR_FUNC void xr_put_idle_m(struct XrRuntime *runtime, XrMachine *m);

// Start or wake an M to run a P
// Reference: Go src/runtime/proc.go startm()
XR_FUNC void xr_startm(struct XrProc *p, bool spinning);

#endif  // XMACHINE_H
