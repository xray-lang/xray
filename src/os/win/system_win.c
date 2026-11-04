/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * system_win.c - Typed system queries and owned temporary process handles
 */
#include "../os_system.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <limits.h>

static XrOsIoStatus system_windows_error(DWORD error) {
    if (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY)
        return XR_OS_IO_OUT_OF_MEMORY;
    return XR_OS_IO_IO;
}

XR_FUNC XrOsIoStatus xr_os_io_uid(XrOsOptionalI64 *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    *output = (XrOsOptionalI64){false, 0};
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_gid(XrOsOptionalI64 *output) {
    return xr_os_io_uid(output);
}

XR_FUNC XrOsIoStatus xr_os_io_system_close(XrOsSystemOwner *owner) {
    if (!owner) return XR_OS_IO_BAD_ARGUMENT;
    if (!owner->handle) return XR_OS_IO_OK;
    if (!CloseHandle((HANDLE)owner->handle)) return system_windows_error(GetLastError());
    owner->handle = 0;
    return XR_OS_IO_OK;
}

/* Close runs even after cancellation. A failed close remains visible to the
 * root owner; it must not trigger an immediate retry or replay the query. */
static XrOsIoStatus system_finish(XrOsSystemOwner *owner, XrOsIoStatus status) {
    XrOsIoStatus closed = xr_os_io_system_close(owner);
    return status == XR_OS_IO_OK ? closed : status;
}

XR_FUNC XrOsIoStatus xr_os_io_ppid(XrOsSystemOwner *owner, void *context,
    bool (*current)(void *context), int64_t *output) {
    if (!owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(context)) return XR_OS_IO_CANCELLED;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return system_windows_error(GetLastError());
    owner->handle = (intptr_t)snapshot;
    XrOsIoStatus status = XR_OS_IO_IO;
    PROCESSENTRY32W entry = {0};
    entry.dwSize = (DWORD)sizeof(entry);
    DWORD self = GetCurrentProcessId();
    int64_t result = 0;
    BOOL found = Process32FirstW(snapshot, &entry);
    while (found) {
        if (current && !current(context)) { status = XR_OS_IO_CANCELLED; break; }
        if (entry.th32ProcessID == self) {
            result = (int64_t)entry.th32ParentProcessID;
            status = XR_OS_IO_OK;
            break;
        }
        found = Process32NextW(snapshot, &entry);
    }
    if (!found) status = system_windows_error(GetLastError());
    status = system_finish(owner, status);
    if (status == XR_OS_IO_OK) *output = result;
    return status;
}

static bool system_process_refused(DWORD error) {
    return error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_PARAMETER ||
        error == ERROR_NOT_FOUND;
}

XR_FUNC XrOsIoStatus xr_os_io_kill(XrOsSystemOwner *owner, int64_t pid,
    int64_t signal, void *context, bool (*current)(void *context), bool *output) {
    if (!owner || owner->handle || !output) return XR_OS_IO_BAD_ARGUMENT;
    if (current && !current(context)) return XR_OS_IO_CANCELLED;
    if (pid <= 0 || (uint64_t)pid > UINT32_MAX || signal <= 0 || signal > INT_MAX) {
        *output = false;
        return XR_OS_IO_OK;
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!process) {
        DWORD error = GetLastError();
        if (!system_process_refused(error)) return system_windows_error(error);
        *output = false;
        return XR_OS_IO_OK;
    }
    owner->handle = (intptr_t)process;
    XrOsIoStatus status = XR_OS_IO_OK;
    bool result = false;
    if (current && !current(context)) status = XR_OS_IO_CANCELLED;
    else {
        /* Windows has no POSIX signal delivery. Match the process abstraction's
         * forced-termination sentinel rather than presenting a signal exit. */
        result = TerminateProcess(process, (UINT)0xE0000001u) != 0;
        if (!result) {
            DWORD error = GetLastError();
            if (!system_process_refused(error)) status = system_windows_error(error);
        }
    }
    status = system_finish(owner, status);
    if (status == XR_OS_IO_OK) *output = result;
    return status;
}

static XrOsIoStatus system_memory(bool available, int64_t *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    MEMORYSTATUSEX memory = {0};
    memory.dwLength = (DWORD)sizeof(memory);
    if (!GlobalMemoryStatusEx(&memory)) return system_windows_error(GetLastError());
    DWORDLONG bytes = available ? memory.ullAvailPhys : memory.ullTotalPhys;
    if (bytes > INT64_MAX) return XR_OS_IO_OVERFLOW;
    *output = (int64_t)bytes;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_total_memory(int64_t *output) {
    return system_memory(false, output);
}
XR_FUNC XrOsIoStatus xr_os_io_free_memory(XrOsSystemOwner *owner, int64_t *output) {
    if (!owner || owner->handle) return XR_OS_IO_BAD_ARGUMENT;
    return system_memory(true, output);
}
XR_FUNC XrOsIoStatus xr_os_io_uptime(double *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    *output = (double)GetTickCount64() / 1000.0;
    return XR_OS_IO_OK;
}
XR_FUNC XrOsIoStatus xr_os_io_loadavg(XrOsLoadAverages *output) {
    if (!output) return XR_OS_IO_BAD_ARGUMENT;
    *output = (XrOsLoadAverages){false, {0.0, 0.0, 0.0}};
    return XR_OS_IO_OK;
}
