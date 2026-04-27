/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_win.c - Windows implementation of os_proc.h.
 *
 * The shim hands callers an int64_t XrProcId. We can't return a
 * raw HANDLE from CreateProcess because it's pointer-sized and not
 * a process identifier. Instead we maintain a tiny intrusive table
 * of (pid, handle) pairs so xr_proc_wait can look the handle back
 * up. The table size is bounded by XR_PROC_MAX_LIVE; once a child
 * is waited-on the slot is freed.
 */

#include "../os_proc.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define XR_PROC_MAX_LIVE 64

static struct {
    DWORD pid;
    HANDLE handle;
} g_live[XR_PROC_MAX_LIVE];
static SRWLOCK g_live_lock = SRWLOCK_INIT;

static void live_record(DWORD pid, HANDLE h) {
    AcquireSRWLockExclusive(&g_live_lock);
    for (int i = 0; i < XR_PROC_MAX_LIVE; i++) {
        if (g_live[i].handle == NULL) {
            g_live[i].pid = pid;
            g_live[i].handle = h;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_live_lock);
}

static HANDLE live_pop(DWORD pid) {
    HANDLE h = NULL;
    AcquireSRWLockExclusive(&g_live_lock);
    for (int i = 0; i < XR_PROC_MAX_LIVE; i++) {
        if (g_live[i].handle != NULL && g_live[i].pid == pid) {
            h = g_live[i].handle;
            g_live[i].handle = NULL;
            g_live[i].pid = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_live_lock);
    return h;
}

// Quote-and-join argv into a single command line. Each argument is
// wrapped in double quotes; embedded quotes and trailing backslashes
// are escaped per Microsoft's CommandLineToArgvW contract.
static char *build_command_line(const char *prog, const char *const argv[]) {
    size_t cap = 0;
    cap += strlen(prog) + 4;
    for (int i = 0; argv[i] != NULL; i++) {
        cap += strlen(argv[i]) * 2 + 4;
    }
    char *buf = (char *) malloc(cap + 1);  // xr:allow-raw-alloc
    if (!buf) {
        return NULL;
    }
    size_t pos = 0;
    // CreateProcess uses argv[0] from the command line; if the
    // caller passed a separate `prog` we put `prog` first.
    int start = 0;
    if (argv[0] != NULL && strcmp(argv[0], prog) == 0) {
        start = 0;  // argv already starts with prog
    } else {
        // Emit prog as the first token explicitly.
        buf[pos++] = '"';
        for (const char *p = prog; *p; p++) {
            if (*p == '"' || *p == '\\') {
                buf[pos++] = '\\';
            }
            buf[pos++] = *p;
        }
        buf[pos++] = '"';
        start = 0;
    }
    for (int i = start; argv[i] != NULL; i++) {
        if (pos > 0) {
            buf[pos++] = ' ';
        }
        buf[pos++] = '"';
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '"' || *p == '\\') {
                buf[pos++] = '\\';
            }
            buf[pos++] = *p;
        }
        buf[pos++] = '"';
    }
    buf[pos] = '\0';
    return buf;
}

XrProcId xr_proc_spawn(const char *prog, const char *const argv[]) {
    if (prog == NULL || argv == NULL) {
        return XR_PROC_INVALID;
    }
    char *cmdline = build_command_line(prog, argv);
    if (!cmdline) {
        return XR_PROC_INVALID;
    }
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    free(cmdline);  // xr:allow-raw-alloc
    if (!ok) {
        return XR_PROC_INVALID;
    }
    CloseHandle(pi.hThread);
    live_record(pi.dwProcessId, pi.hProcess);
    return (XrProcId) pi.dwProcessId;
}

int xr_proc_wait(XrProcId pid, int *exit_code) {
    if (pid <= 0) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    HANDLE h = live_pop((DWORD) pid);
    if (h == NULL) {
        // Fall back to OpenProcess so callers from a different
        // path still get a meaningful answer.
        h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, (DWORD) pid);
        if (h == NULL) {
            if (exit_code) {
                *exit_code = -1;
            }
            return -1;
        }
    }
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    BOOL ok = GetExitCodeProcess(h, &code);
    CloseHandle(h);
    if (!ok) {
        if (exit_code) {
            *exit_code = -1;
        }
        return -1;
    }
    if (exit_code) {
        *exit_code = (int) code;
    }
    return 0;
}

int64_t xr_proc_self_pid(void) {
    return (int64_t) GetCurrentProcessId();
}

bool xr_proc_debugger_attached(void) {
    return IsDebuggerPresent() ? true : false;
}
