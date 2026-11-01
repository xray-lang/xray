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
#ifndef XR_OS_WINDOWS
    /* Observed size and modification/change times; reads must find them again. */
    uint64_t size;
    int64_t changed[4];
#endif
};
#ifdef XR_OS_WINDOWS
#include <windows.h>
#include "xtc_xir_sysroot_windows.inc.c"
#include "xtc_xir_hash_proof.inc.c"
#include "xtc_xir_file_lease_windows.inc.c"
#else
#include "xtc_xir_file_lease_posix.inc.c"
#endif
XR_FUNC XrXirTargetStatus xtc_xir_file_lease_read(XtcXirFileLease *lease, size_t limit,
    void **owned_bytes, size_t *length) {
    if (!lease || lease->directory) return XR_XIR_TARGET_INVALID;
#ifdef XR_OS_WINDOWS
    return xtc_xir_sysroot_read(&lease->storage, lease->lock, lease->file.length, limit, owned_bytes, length);
#else
    return file_lease_posix_read(lease, limit, owned_bytes, length);
#endif
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
