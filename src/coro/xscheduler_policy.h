/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xscheduler_policy.h - Internal scheduler policy hooks
 */

#ifndef XSCHEDULER_POLICY_H
#define XSCHEDULER_POLICY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "xworker.h"

XR_FUNC int xr_sched_effective_priority_for_coro(XrCoroutine *coro, int64_t now);
XR_FUNC XrCoroutine *xr_worker_try_steal_once(XrWorker *worker, XrRuntime *runtime,
                                              _Atomic bool *running_ptr, int64_t *out_delay_hint,
                                              bool *should_exit);

#endif /* XSCHEDULER_POLICY_H */
