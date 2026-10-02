/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_self_unix.c - Current-process queries without spawning dependencies
 */
#include "../os_proc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(XR_OS_MACOS)
#include <limits.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

int64_t xr_proc_self_pid(void) {
    return (int64_t) getpid();
}

int xr_proc_self_exe_path(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }
#if defined(XR_OS_MACOS)
    char raw[PATH_MAX];
    uint32_t raw_size = (uint32_t) sizeof(raw);
    if (_NSGetExecutablePath(raw, &raw_size) != 0) {
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
    ssize_t n = readlink("/proc/self/exe", buf, size - 1);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    return 0;
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
