/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_runtime.h - AOT runtime lifecycle (init / shutdown)
 *
 * KEY CONCEPT:
 *   Generated main() owns one XrtRuntime instance. Lifecycle is:
 *     1. XrtRuntime rt = {0};
 *     2. xrt_runtime_init(&rt);
 *     3. <module-1>__module_init(&rt);
 *        <module-2>__module_init(&rt);
 *        ...
 *        <entry>__module_init(&rt);
 *     4. xrt_runtime_shutdown(&rt);
 *
 *   The runtime pointer flows into every generated function as the
 *   XrtContext xrt_ctx argument (xrt_compat.h defines that as void*),
 *   replacing the historical NULL placeholder.
 *
 *   Today the runtime struct is intentionally minimal — it only owns the
 *   ARC + bump allocator lifecycle. It is the anchor for future
 *   owned state (type_table, exception frame stack root, dtor
 *   invocation, container ARC integration). New owned state must live
 *   HERE, not in ad-hoc globals.
 *
 * STATUS NOTE:
 *   bump allocator state and the type table currently live as file-local
 *   globals in xrt_arc.h / xrt_class.h. They will eventually be folded
 *   into this struct; until then, init/shutdown drives those globals
 *   via existing helpers (xrt_arc_init / xrt_bump_destroy).
 *
 * RELATED MODULES:
 *   - xrt_arc.h     : bump allocator + ARC primitives
 *   - xcgen.c       : emits per-function signatures with XrtContext xrt_ctx
 *   - xcmd_build.c  : aot_write_main() emits the runtime lifecycle calls
 */

#ifndef XRT_RUNTIME_H
#define XRT_RUNTIME_H

#include "xrt_arc.h"

/* =========================================================================
 * XrtRuntime — root of the AOT runtime instance
 *
 * Currently holds only an `initialized` flag; future revisions will
 * gain an owned type table, exception frame stack head, allocator
 * policy selector, etc. The struct layout is kept stable so generated
 * main() stays forward-compatible.
 * ========================================================================= */

typedef struct XrtRuntime {
    int initialized; /* 0 = pristine, 1 = init done */
} XrtRuntime;

/* xrt_runtime_init — set up allocators and any future global state.
 * Idempotent: safe to call twice on the same instance. */
static inline void xrt_runtime_init(XrtRuntime *rt) {
    if (rt && rt->initialized)
        return;
    xrt_arc_init(); /* primes bump arena iff xrt_bump_enabled */
    if (rt)
        rt->initialized = 1;
}

/* xrt_runtime_shutdown — tear down allocators owned by the runtime.
 * Safe to call on a never-initialized instance (no-op). */
static inline void xrt_runtime_shutdown(XrtRuntime *rt) {
    if (rt && !rt->initialized)
        return;
    if (xrt_bump_enabled)
        xrt_bump_destroy();
    if (rt)
        rt->initialized = 0;
}

#endif /* XRT_RUNTIME_H */
