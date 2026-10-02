/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_self_unix.c - Current-process queries without spawning dependencies
 */
#include "../os_proc.h"
#include "../../base/xfileio.h"
#include "../../base/xio_policy.inc.h"
#include "../../shared/xr_utf8_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(XR_OS_MACOS)
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

static int proc_self_utf8_read(void *context, const uint8_t *address, uint8_t *output) {
    XrIoContext *io = context;
    if (!io_work(io, 1)) return 0;
    *output = *address;
    return 1;
}
static bool proc_self_utf8(XrIoContext *io, const char *text, size_t *length) {
    if (!io_length(io, text, length)) return false;
    XrUtf8ScanResult result = {0};
    if (!xr_utf8_core_scan_strict_read((const uint8_t *)text, *length, proc_self_utf8_read, io, &result))
        return false;
    return result.error == XR_UTF8_OK || io_status(io, XR_OS_IO_BAD_ARGUMENT);
}

static XrOsIoStatus proc_self_executable(char *buffer, size_t capacity) {
#if defined(XR_OS_MACOS)
    if (capacity > UINT32_MAX) return XR_OS_IO_BUDGET;
    uint32_t size = (uint32_t)capacity;
    return _NSGetExecutablePath(buffer, &size) == 0 ? XR_OS_IO_OK : XR_OS_IO_BUDGET;
#elif defined(XR_OS_LINUX)
    ssize_t count = readlink("/proc/self/exe", buffer, capacity);
    if (count < 0) return io_errno_status(errno);
    if (!count) return XR_OS_IO_IO;
    if ((size_t)count >= capacity) return XR_OS_IO_BUDGET;
    buffer[count] = 0;
    return XR_OS_IO_OK;
#else
    (void)buffer; (void)capacity;
    return XR_OS_IO_UNSUPPORTED;
#endif
}

XR_FUNC XrOsIoStatus xr_os_io_self_exe_path(const XrOsIoPolicy *policy, char **output) {
    if (!io_policy_valid(policy) || !output || *output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    char *raw = io_alloc(&io, XR_PATH_MAX), *result = NULL;
    if (raw && io_work(&io, 1 + XR_PATH_MAX))
        io_status(&io, proc_self_executable(raw, XR_PATH_MAX));
    size_t length = 0;
    if (io.status == XR_OS_IO_OK) (void)proc_self_utf8(&io, raw, &length);
    if (io.status == XR_OS_IO_OK) {
#if defined(XR_OS_MACOS)
        io_status(&io, xr_realpath_owned(policy, raw, &result));
        if (io.status == XR_OS_IO_OK) (void)proc_self_utf8(&io, result, &length);
#else
        result = raw; raw = NULL;
#endif
    }
    io_free(&io, raw);
    if (io.status == XR_OS_IO_OK) *output = result;
    else io_free(&io, result);
    return io.status;
}

XR_FUNC XrOsIoStatus xr_os_io_environment_get(const XrOsIoPolicy *policy,
    const char *name, char **output) {
    if (!io_policy_valid(policy) || !name || !output || *output) return XR_OS_IO_BAD_ARGUMENT;
    XrIoContext io = {policy, XR_OS_IO_OK};
    size_t length = 0;
    if (!proc_self_utf8(&io, name, &length)) return io.status;
    if (!length) return XR_OS_IO_BAD_ARGUMENT;
    for (size_t i = 0; i < length; ++i) {
        if (!io_work(&io, 1)) return io.status;
        if (name[i] == '=') return XR_OS_IO_BAD_ARGUMENT;
    }
    if (!io_work(&io, 1)) return io.status;
    const char *value = getenv(name);
    if (!value) return XR_OS_IO_NOT_FOUND;
    if (!proc_self_utf8(&io, value, &length)) return io.status;
    char *result = io_alloc(&io, length + 1);
    if (result) (void)io_copy(&io, result, value, length + 1);
    if (io.status == XR_OS_IO_OK) *output = result;
    else io_free(&io, result);
    return io.status;
}

int64_t xr_proc_self_pid(void) {
    return (int64_t) getpid();
}

int xr_proc_self_exe_path(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }
#if defined(XR_OS_MACOS)
    char raw[PATH_MAX];
    if (proc_self_executable(raw, sizeof(raw)) != XR_OS_IO_OK) {
        return -1;
    }
    char resolved[PATH_MAX];
    const char *src = realpath(raw, resolved) ? resolved : raw;
    size_t len = strlen(src);
    if (len + 1 > size) {
        return -1;
    }
    memcpy(buf, src, len + 1);
    return 0;
#elif defined(XR_OS_LINUX)
    return proc_self_executable(buf, size) == XR_OS_IO_OK ? 0 : -1;
#else
    /* BSDs vary (sysctl KERN_PROC_PATHNAME vs /proc); no in-tree
     * caller targets them yet, so report unsupported rather than
     * guess a wrong path. */
    return -1;
#endif
}

bool xr_proc_debugger_attached(void) {
#if defined(XR_OS_MACOS)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    memset(&info, 0, sizeof(info));
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) {
        return false;
    }
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#elif defined(XR_OS_LINUX)
    // /proc/self/status has a "TracerPid:\t<n>\n" line; non-zero
    // means a debugger is attached.
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) {
        return false;
    }
    char line[256];
    bool attached = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            const char *p = line + 10;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            if (*p && *p != '0') {
                attached = true;
            }
            break;
        }
    }
    fclose(f);
    return attached;
#else
    return false;
#endif
}
