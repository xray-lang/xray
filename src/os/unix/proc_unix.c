/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_unix.c - POSIX implementation of os_proc.h.
 */

#include "../os_proc.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(XR_OS_MACOS)
#include <string.h>
#include <sys/sysctl.h>
#elif defined(XR_OS_LINUX)
#include <stdio.h>
#include <string.h>
#endif

XrProcId xr_proc_spawn(const char *prog, const char *const argv[]) {
    if (prog == NULL || argv == NULL) {
        return XR_PROC_INVALID;
    }
    pid_t pid = fork();
    if (pid < 0) {
        return XR_PROC_INVALID;
    }
    if (pid == 0) {
        // Child: exec, never returns on success.
        execvp(prog, (char *const *) argv);
        // exec failed; emit a short message and exit with 127 so
        // the parent's wait observes the failure. 127 matches the
        // shell convention for "command not found".
        _exit(127);
    }
    return (XrProcId) pid;
}

int xr_proc_wait(XrProcId pid, int *exit_code) {
    if (pid <= 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    int status = 0;
    pid_t r;
    do {
        r = waitpid((pid_t) pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    if (exit_code) {
        if (WIFEXITED(status)) {
            *exit_code = WEXITSTATUS(status);
        } else {
            *exit_code = -1;
        }
    }
    return 0;
}

int64_t xr_proc_self_pid(void) {
    return (int64_t) getpid();
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
