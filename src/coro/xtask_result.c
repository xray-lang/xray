/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtask_result.c - Task result handoff into an awaiter's heap.
 */

#include "xtask.h"
#include "xdeep_copy.h"

static inline void task_result_lock_acquire(_Atomic bool *lock) {
    while (atomic_exchange_explicit(lock, true, memory_order_acquire)) {
    }
}

static inline void task_result_lock_release(_Atomic bool *lock) {
    atomic_store_explicit(lock, false, memory_order_release);
}

XrValue xr_task_consume_result_copy(struct XrRuntimeCore *core, XrTask *task,
                                    struct XrCoroutine *dst_coro) {
    /* Serialize concurrent awaiters of the same task: the first one deep
     * copies the result out of the executor heap and caches the copy back
     * into the task (so later awaiters never chase the recycled executor
     * heap). XrValue is 16 bytes; without the lock a reader could observe
     * a torn cache write. */
    task_result_lock_acquire(&task->await_lock);
    XrValue result = task->result;
    if (xr_value_needs_copy(result)) {
        XrValue copy = xr_deep_copy_to_coro_core(core, result, dst_coro);
        task->result = copy;
        result = copy;
    }
    task_result_lock_release(&task->await_lock);
    return result;
}
