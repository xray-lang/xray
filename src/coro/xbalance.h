/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbalance.h - Load balancing definitions
 *
 * KEY CONCEPT:
 *   Reduction-based load balance checking, migration paths,
 *   active migration (emigrate/immigrate).
 */

#ifndef XBALANCE_H
#define XBALANCE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "../base/xdefs.h"
#include "../base/xconstants.h"

// Forward declarations
struct XrWorker;
struct XrRuntime;
struct XrCoroutine;

// ========== Constants ==========

// Load balance check interval (reductions)
#define XR_CHECK_BALANCE_REDS_PER_WORKER (2000 * XR_CORO_REDUCTIONS)  // 8,000,000
#define XR_CALL_CHECK_BALANCE_REDS (XR_CHECK_BALANCE_REDS_PER_WORKER / 2)  // 4,000,000

// Migration threshold constants
#define XR_MIGRATION_LIMIT_DEFAULT 100  // Default migration threshold
#define XR_MIGRATION_MAX_STEAL 50       // Max steal per operation

// Load balance history size
#define XR_FULL_REDS_HISTORY_SIZE 4

// ========== Migration Path Structures ==========

// Per-priority migration limit
typedef struct {
    int limit_here;     // Local queue threshold (emigrate if exceeded)
    int limit_other;    // Target queue threshold (immigrate if below)
    int target_worker;  // Target Worker ID (-1 = none)
} XrMigrationLimit;

// Worker migration path
typedef struct {
    uint32_t flags;
    XrMigrationLimit prio[3];
} XrMigrationPath;

// Migration flags
#define XR_MIG_FLG_EMIGRATE_LOW    (1 << 0)
#define XR_MIG_FLG_EMIGRATE_NORMAL (1 << 1)
#define XR_MIG_FLG_EMIGRATE_HIGH   (1 << 2)
#define XR_MIG_FLG_IMMIGRATE_LOW   (1 << 4)
#define XR_MIG_FLG_IMMIGRATE_NORMAL (1 << 5)
#define XR_MIG_FLG_IMMIGRATE_HIGH  (1 << 6)
#define XR_MIG_FLG_OUT_OF_WORK     (1 << 8)

// ========== Global Load Balance State ==========

// Load balance global state
typedef struct {
    _Atomic int checking_balance;            // Checking flag (prevent concurrency)
    int last_active_workers;
    int halftime;
    int full_reds_history_index;
} XrBalanceInfo;

// ========== API ==========

// Check and perform load balancing
XR_FUNC void xr_check_balance(struct XrRuntime *runtime, struct XrWorker *worker);

// Try to emigrate coroutines to other Workers
XR_FUNC int xr_try_emigrate(struct XrWorker *worker);

// Choose target Worker (lowest load)
XR_FUNC struct XrWorker *xr_choose_target_worker(struct XrRuntime *runtime, int exclude);

// Initialize load balance module
XR_FUNC void xr_balance_init(struct XrRuntime *runtime);

// Record coroutine executed reductions
XR_FUNC void xr_worker_reductions_executed(struct XrWorker *worker, int prio, int reds);

// Get run queue statistics
XR_FUNC void xr_runq_get_info(struct XrWorker *worker, int prio, int *len, int *reds);

#endif // XBALANCE_H
