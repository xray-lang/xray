/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_sysroot.c - Same-handle input hashes and read-only path leases
 *
 * KEY CONCEPT:
 *   Leases protect observed files; directory leases do not freeze search results.
 */
#include "xtc_xir_sysroot_internal.h"
#include "../../base/xsha256.h"
#include "../../base/xchecks.h"
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include "xtc_xir_sysroot_windows.inc.c"
#else
XR_FUNC bool xtc_xir_sysroot_capture(XrXirTargetSnapshot *snapshot, const XrXirTargetRequest *request) {
    (void)request;
    return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_UNSUPPORTED);
}
XR_FUNC void xtc_xir_sysroot_close(XrXirTargetSnapshot *snapshot) { (void)snapshot; }
XR_FUNC bool xtc_xir_sysroot_observe(XrXirImageCollector *images, const XrProcImageEvent *event) {
    (void)event; return xtc_xir_target_fail(&images->storage, XR_XIR_TARGET_UNSUPPORTED);
}
XR_FUNC bool xtc_xir_sysroot_hash(XrXirTargetSnapshot *snapshot, XtcXirLock *lock, XrXirTargetFile *file) {
    (void)lock; (void)file; return xtc_xir_target_fail(snapshot, XR_XIR_TARGET_UNSUPPORTED);
}
#endif
