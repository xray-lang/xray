/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmachine.c - Machine (M/OS thread) implementation for P/M split scheduler
 *
 * KEY CONCEPT:
 *   M lifecycle: alloc -> init -> park (wait for P) -> acquire P -> run -> release P -> park
 *   When M blocks in C code, it releases P via handoff, stays blocked, then parks after return.
 */

#include "xmachine.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "xproc.h"
#include "xworker.h"
#include "../runtime/xstrbuf.h"
#include "xcoro_tuning.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ========== M Lifecycle ==========

XrMachine *xr_machine_alloc(struct XrRuntime *runtime, int id) {
    XR_DCHECK(runtime != NULL, "machine_alloc: NULL runtime");
    XrMachine *m = (XrMachine *) xr_calloc(1, sizeof(XrMachine));
    if (!m)
        return NULL;
    xr_machine_init(m, id, runtime);
    return m;
}

void xr_machine_init(XrMachine *m, int id, struct XrRuntime *runtime) {
    XR_DCHECK(m != NULL, "machine_init: NULL machine");
    XR_DCHECK(runtime != NULL, "machine_init: NULL runtime");
    memset(m, 0, sizeof(XrMachine));
    m->id = id;
    m->runtime = runtime;
    atomic_store(&m->current_p, NULL);
    atomic_store(&m->state, M_PARKED);
    m->spinning = false;
    atomic_store_explicit(&m->current_coro, (struct XrCoroutine *) NULL, memory_order_relaxed);
    m->tmp_strbuf = NULL;
    m->backend_storage = NULL;
    m->backend_storage_destroy = NULL;
    m->all_link = NULL;
    atomic_store_explicit(&m->idle_link, NULL, memory_order_relaxed);
    atomic_store(&m->in_idle_worker_list, false);
    atomic_store(&m->heartbeat, 0);

    // Initialize futex-based park state
    atomic_store(&m->park_state, XR_PARK_IDLE);
}

void xr_machine_destroy(XrMachine *m) {
    if (!m)
        return;

    // Free string buffer
    if (m->tmp_strbuf) {
        xr_strbuf_free(m->tmp_strbuf);
        m->tmp_strbuf = NULL;
    }

    if (m->backend_storage_destroy && m->backend_storage) {
        m->backend_storage_destroy(m->backend_storage);
        m->backend_storage = NULL;
    }
    m->backend_storage_destroy = NULL;
}
