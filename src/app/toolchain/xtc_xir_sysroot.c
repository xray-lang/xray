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
#include "xtc_xir_file_lease.h"
#include "../../base/xsha256.h"
#include "../../base/xchecks.h"
#include <string.h>
struct XtcXirFileLease {
    XrXirTargetSnapshot storage;
    XtcXirLock *lock;
    bool directory;
    XtcXirFileFacts file;
    XtcXirDirectoryFacts directory_facts;
};
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include "xtc_xir_sysroot_windows.inc.c"
#include "xtc_xir_file_lease_windows.inc.c"
#else
XR_FUNC bool xtc_xir_sysroot_capture(XrXirTargetSnapshot *snapshot, const XrXirTargetSnapshotRequest *request) {
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
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output) {
    if (!resources || !path || !output || *output) return XR_XIR_TARGET_INVALID;
    return XR_XIR_TARGET_UNSUPPORTED;
}
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_directory_open(XrCompileResources *resources,
    const char *path, XtcXirFileLease **output) {
    return xtc_xir_file_lease_open(resources, path, output);
}
XR_FUNC XrXirTargetStatus xtc_xir_sysroot_read(XrXirTargetSnapshot *storage,
    XtcXirLock *lock, uint64_t file_length, size_t limit, void **owned_bytes, size_t *length) {
    (void)file_length;
    if (!storage || !lock || lock->directory || !limit || !owned_bytes || *owned_bytes || !length || *length)
        return XR_XIR_TARGET_INVALID;
    return XR_XIR_TARGET_UNSUPPORTED;
}
#endif
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_read(XtcXirFileLease *lease, size_t limit,
    void **owned_bytes, size_t *length) {
    if (!lease || lease->directory) return XR_XIR_TARGET_INVALID;
    return xtc_xir_sysroot_read(&lease->storage, lease->lock, lease->file.length, limit, owned_bytes, length);
}
XR_FUNC XrCompileResources *xtc_xir_file_lease_resources(const XtcXirFileLease *lease) {
    return lease ? lease->storage.resources : NULL;
}
XR_FUNC const XtcXirFileFacts *xtc_xir_file_lease_facts(const XtcXirFileLease *lease) {
    return lease && !lease->directory ? &lease->file : NULL;
}
XR_FUNC const XtcXirDirectoryFacts *xtc_xir_file_lease_directory_facts(const XtcXirFileLease *lease) {
    return lease && lease->directory ? &lease->directory_facts : NULL;
}
XR_FUNC void xtc_xir_file_lease_free(XtcXirFileLease *lease) {
    if (!lease) return;
    xtc_xir_sysroot_close(&lease->storage);
    while (lease->storage.memory) {
        XtcXirMemory *memory = lease->storage.memory; lease->storage.memory = memory->next;
        xr_compile_resources_free(memory);
    }
    xr_compile_resources_free(lease);
}
