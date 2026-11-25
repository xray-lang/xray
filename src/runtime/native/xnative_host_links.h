/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xnative_host_links.h - Read a stored link target or resolve a named path
 */
#ifndef XNATIVE_HOST_LINKS_H
#define XNATIVE_HOST_LINKS_H
#include "xnative_host_nullable.h"
#include "os/os_file_links.h"

typedef struct XrtHostLinkResult {
    XrtManagedStatus managed;
    XrOsIoStatus host;
    bool present;
} XrtHostLinkResult;

/* One path query publishes its textual answer as one owned String in the
 * validated non-null branch, or leaves the validated empty optional when the
 * host has no answer (a missing path, a leaf that is not a link, or a platform
 * without a proven bound). A transport that could not allocate cannot promise
 * the empty optional, so its status reaches the caller instead. `follow`
 * selects realpath over readlink. The receiving binding must already exist;
 * xrt_execution_link_result enforces that before any host effect. */
XR_FUNC XrtHostLinkResult xrt_host_link_text_publish(const XrtHostNullableSlots *,
    const XrtManagedHeader *path, bool follow, uint32_t source_id);
#endif // XNATIVE_HOST_LINKS_H
