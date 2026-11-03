/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * file_temp_unix.c - Explicit boundary for unqualified temporary ownership
 *
 * KEY CONCEPT:
 *   A pathname check followed by unlink is not atomic inode-bound rollback.
 *   No mutation is attempted under an unproved ownership/close protocol.
 */
#include "../os_file_temp.h"
#include "../../base/xio_policy.inc.h"
static XrOsIoStatus file_temp_unavailable(const XrOsFileTempRequest *request,
    XrOsFileTempOwner *owner,bool *created) {
    if(!request || !owner || owner->state || !created || !request->prepare ||
        !request->root.text || !io_policy_valid(request->policy))return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_UNSUPPORTED;
}
XR_FUNC XrOsIoStatus xr_os_io_temp_directory(const XrOsFileTempRequest *request,XrOsFileTempOwner *owner,bool *created) {
    return file_temp_unavailable(request,owner,created);
}
XR_FUNC XrOsIoStatus xr_os_io_temp_file(const XrOsFileTempRequest *request,XrOsFileTempOwner *owner,bool *created) {
    return file_temp_unavailable(request,owner,created);
}
XR_FUNC XrOsIoStatus xr_os_io_temp_finish(const XrOsIoPolicy *policy,XrOsFileTempOwner *owner,bool keep) {
    (void)keep;
    if(!owner || owner->state || !io_policy_valid(policy))return XR_OS_IO_BAD_ARGUMENT;
    return XR_OS_IO_OK;
}
