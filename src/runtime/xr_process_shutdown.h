/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_process_shutdown.h - process-wide registry teardown (task 218 line 4).
 */

#ifndef XR_PROCESS_SHUTDOWN_H
#define XR_PROCESS_SHUTDOWN_H

#include "../base/xdefs.h"

/*
 * Release process-wide, non-isolate-owned registries so a leak checker
 * (LeakSanitizer) observes a clean exit.
 *
 * This is the single, idempotent teardown hook for defense line 4. It is
 * intended for TEST and TOOL builds (unit tests, the stdlib bytecode
 * generator, CLI tools) that want a leak-clean process exit; a normal
 * long-lived process may simply exit and let the OS reclaim everything.
 *
 * It does NOT free per-isolate state — that is owned by XrRuntimeCore and
 * released by xray_vm_delete(). After task 213 moved the region L2 block
 * pool to per-isolate ownership, the runtime's genuinely process-global
 * surface is intentionally small; this hook is the one place to release it
 * and the anchor point as more process-global registries are migrated here.
 *
 * Safe to call multiple times and from any single-threaded shutdown path.
 */
XR_FUNC void xr_process_shutdown(void);

#endif /* XR_PROCESS_SHUTDOWN_H */
