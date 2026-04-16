/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xworker_internal.h - Internal shared declarations for worker implementation
 *
 * KEY CONCEPT:
 *   Shared includes, TLS variables, and internal helper declarations
 *   used by xworker.c and xworker_sysmon.c.
 */
#ifndef XWORKER_INTERNAL_H
#define XWORKER_INTERNAL_H

#include "xworker.h"
#include "../vm/xvm_internal.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstring.h"
#include "xchannel.h"
#include "xcoro_pool.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../runtime/gc/xsystem_heap.h"
#include "xasync.h"
#include "xbalance.h"
#include "xtimer_wheel.h"
#include "xyieldable.h"
#include "xresume.h"
#include "../runtime/object/xexception.h"
#include "../runtime/xray_debug.h"
#include "xcoro_registry.h"

// TLS variables (defined in xworker.c)
extern __thread XrWorker *tls_current_worker;
extern __thread XrMachine *tls_current_machine;

// Internal helpers (defined in xworker.c)
XR_FUNC int64_t get_current_time_us(void);
XR_FUNC void wake_idle_worker(XrRuntime *rt);
XR_FUNC void worker_unpark(XrWorker *worker);
XR_FUNC void *worker_loop(void *arg);
XR_FUNC bool worker_blocked_list_remove(XrWorker *worker, XrCoroutine *coro);
XR_FUNC void worker_blocked_list_add(XrWorker *worker, XrCoroutine *coro);
XR_FUNC XrBlockedBucket *worker_blocked_bucket_find_or_create(XrWorker *worker, void *channel);
XR_FUNC XrBlockedBucket *worker_blocked_bucket_find(XrWorker *worker, void *channel);

// Sysmon constants
#define XR_SYSMON_WARN_US       100000
#define XR_SYSMON_CANCEL_US     5000000
#define XR_SYSMON_STEAL_US      1000

// Sysmon function (defined in xworker_sysmon.c)
XR_FUNC void *sysmon_thread_func(void *arg);

#endif // XWORKER_INTERNAL_H
