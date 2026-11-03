/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_system_queries.c - Real system queries and temporary-handle failures
 */
#include "os/os_system.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "system query %d: %s\n", __LINE__, #condition); return 0; \
} } while (0)

#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

static size_t close_calls, close_failures;
static DWORD memory_failure, snapshot_failure;
static bool memory_overflow;
static MEMORYSTATUSEX memory_reference;
static BOOL WINAPI observed_close(HANDLE handle) {
    ++close_calls;
    if (close_failures) {
        --close_failures;
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return CloseHandle(handle);
}
static BOOL WINAPI observed_memory(LPMEMORYSTATUSEX memory) {
    if (memory_failure) { SetLastError(memory_failure); return FALSE; }
    BOOL queried = GlobalMemoryStatusEx(memory);
    if (queried && memory_overflow) memory->ullTotalPhys = UINT64_MAX;
    if (queried) memory_reference = *memory;
    return queried;
}
static HANDLE WINAPI observed_snapshot(DWORD flags, DWORD process) {
    if (snapshot_failure) { SetLastError(snapshot_failure); return INVALID_HANDLE_VALUE; }
    return CreateToolhelp32Snapshot(flags, process);
}
#define CloseHandle observed_close
#define GlobalMemoryStatusEx observed_memory
#define CreateToolhelp32Snapshot observed_snapshot
#include "os/win/system_win.c"
#undef CreateToolhelp32Snapshot
#undef GlobalMemoryStatusEx
#undef CloseHandle

typedef struct QueryControl { size_t calls, cancel_at; } QueryControl;
static bool query_current(void *context) {
    QueryControl *control = context;
    return ++control->calls < control->cancel_at;
}

static int verify_queries(void) {
    XrOsSystemOwner owner = {0};
    XrOsOptionalI64 identity = {true, 123};
    CHECK(xr_os_io_uid(&identity) == XR_OS_IO_OK && !identity.present);
    identity = (XrOsOptionalI64){true, 456};
    CHECK(xr_os_io_gid(&identity) == XR_OS_IO_OK && !identity.present);
    CHECK(xr_os_io_uid(NULL) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_gid(NULL) == XR_OS_IO_BAD_ARGUMENT);
    int64_t bytes = -321;
    CHECK(xr_os_io_total_memory(&bytes) == XR_OS_IO_OK && bytes > 0 &&
        (uint64_t)bytes == memory_reference.ullTotalPhys);
    CHECK(xr_os_io_free_memory(&owner, &bytes) == XR_OS_IO_OK && !owner.handle && bytes >= 0 &&
        (uint64_t)bytes == memory_reference.ullAvailPhys);
    bytes = -321;
    memory_failure = ERROR_NOT_ENOUGH_MEMORY;
    CHECK(xr_os_io_free_memory(&owner, &bytes) == XR_OS_IO_OUT_OF_MEMORY && !owner.handle && bytes == -321);
    memory_failure = ERROR_ACCESS_DENIED;
    CHECK(xr_os_io_total_memory(&bytes) == XR_OS_IO_IO && bytes == -321);
    memory_failure = ERROR_SUCCESS;
    memory_overflow = true;
    CHECK(xr_os_io_total_memory(&bytes) == XR_OS_IO_OVERFLOW && bytes == -321);
    memory_overflow = false;
    CHECK(xr_os_io_total_memory(NULL) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_free_memory(&owner, NULL) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_free_memory(NULL, &bytes) == XR_OS_IO_BAD_ARGUMENT && bytes == -321);
    owner.handle = 1;
    CHECK(xr_os_io_free_memory(&owner, &bytes) == XR_OS_IO_BAD_ARGUMENT && owner.handle == 1 && bytes == -321);
    owner.handle = 0;
    double before = (double)GetTickCount64() / 1000.0, seconds = -1.0;
    CHECK(xr_os_io_uptime(&seconds) == XR_OS_IO_OK && seconds >= before &&
        seconds <= (double)GetTickCount64() / 1000.0);
    CHECK(xr_os_io_uptime(NULL) == XR_OS_IO_BAD_ARGUMENT);
    XrOsLoadAverages loads = {true, {1.0, 2.0, 3.0}};
    CHECK(xr_os_io_loadavg(&loads) == XR_OS_IO_OK && !loads.available);
    CHECK(xr_os_io_loadavg(NULL) == XR_OS_IO_BAD_ARGUMENT);
    return 1;
}

static int verify_parent(int64_t expected) {
    XrOsSystemOwner owner = {0};
    int64_t parent = -123;
    size_t before = close_calls;
    CHECK(xr_os_io_ppid(&owner, NULL, NULL, &parent) == XR_OS_IO_OK &&
        parent == expected && !owner.handle && close_calls == before + 1);
    parent = -123;
    snapshot_failure = ERROR_NOT_ENOUGH_MEMORY;
    before = close_calls;
    CHECK(xr_os_io_ppid(&owner, NULL, NULL, &parent) == XR_OS_IO_OUT_OF_MEMORY &&
        parent == -123 && !owner.handle && close_calls == before);
    snapshot_failure = ERROR_SUCCESS;
    close_failures = 1;
    CHECK(xr_os_io_ppid(&owner, NULL, NULL, &parent) == XR_OS_IO_IO &&
        owner.handle && parent == -123 && close_calls == before + 1);
    intptr_t saved = owner.handle;
    CHECK(xr_os_io_ppid(&owner, NULL, NULL, &parent) == XR_OS_IO_BAD_ARGUMENT &&
        owner.handle == saved && parent == -123 && close_calls == before + 1);
    CHECK(xr_os_io_system_close(&owner) == XR_OS_IO_OK && !owner.handle &&
        close_calls == before + 2);
    CHECK(xr_os_io_system_close(&owner) == XR_OS_IO_OK && close_calls == before + 2);
    QueryControl control = {0, 1};
    before = close_calls;
    CHECK(xr_os_io_ppid(&owner, &control, query_current, &parent) == XR_OS_IO_CANCELLED &&
        parent == -123 && !owner.handle && close_calls == before);
    control = (QueryControl){0, 2};
    CHECK(xr_os_io_ppid(&owner, &control, query_current, &parent) == XR_OS_IO_CANCELLED &&
        parent == -123 && !owner.handle && close_calls == before + 1);
    control = (QueryControl){0, 2};
    close_failures = 1;
    CHECK(xr_os_io_ppid(&owner, &control, query_current, &parent) == XR_OS_IO_CANCELLED &&
        parent == -123 && owner.handle && close_calls == before + 2);
    CHECK(xr_os_io_system_close(&owner) == XR_OS_IO_OK && !owner.handle);
    CHECK(xr_os_io_ppid(NULL, NULL, NULL, &parent) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_ppid(&owner, NULL, NULL, NULL) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_system_close(NULL) == XR_OS_IO_BAD_ARGUMENT);
    return 1;
}

static int spawn_self(const wchar_t *arguments, PROCESS_INFORMATION *process) {
    wchar_t executable[32768], command[32768];
    DWORD length = GetModuleFileNameW(NULL, executable, 32768);
    if (!length || length >= 32768) return 0;
    int count = swprintf(command, 32768, L"\"%ls\" %ls", executable, arguments);
    if (count < 0 || count >= 32768) return 0;
    STARTUPINFOW startup = {0};
    startup.cb = (DWORD)sizeof(startup);
    memset(process, 0, sizeof(*process));
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
        NULL, NULL, &startup, process)) return 0;
    CloseHandle(process->hThread);
    process->hThread = NULL;
    return 1;
}

static int verify_child_parent(void) {
    wchar_t arguments[64];
    int count = swprintf(arguments, 64, L"--parent %lu", (unsigned long)GetCurrentProcessId());
    CHECK(count > 0 && count < 64);
    PROCESS_INFORMATION process;
    CHECK(spawn_self(arguments, &process));
    DWORD waited = WaitForSingleObject(process.hProcess, 10000), code = UINT32_MAX;
    bool queried = GetExitCodeProcess(process.hProcess, &code) != 0;
    if (waited != WAIT_OBJECT_0) {
        (void)TerminateProcess(process.hProcess, 99);
        (void)WaitForSingleObject(process.hProcess, 10000);
    }
    bool closed = CloseHandle(process.hProcess) != 0;
    CHECK(waited == WAIT_OBJECT_0 && queried && !code && closed);
    return 1;
}

static int verify_kill(void) {
    PROCESS_INFORMATION process;
    CHECK(spawn_self(L"--sleep", &process));
    int passed = 0;
    XrOsSystemOwner owner = {0};
    bool result = true;
    QueryControl control = {0, 1};
#define KILL_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "kill %d: %s\n", __LINE__, #condition); goto cleanup; \
} } while (0)
    size_t before = close_calls;
    KILL_CHECK(xr_os_io_kill(&owner, process.dwProcessId, 15, &control, query_current, &result) ==
        XR_OS_IO_CANCELLED && result && !owner.handle && close_calls == before);
    control = (QueryControl){0, 2};
    KILL_CHECK(xr_os_io_kill(&owner, process.dwProcessId, 15, &control, query_current, &result) ==
        XR_OS_IO_CANCELLED && result && !owner.handle && close_calls == before + 1);
    KILL_CHECK(WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT);
    control = (QueryControl){0, 2};
    close_failures = 1;
    KILL_CHECK(xr_os_io_kill(&owner, process.dwProcessId, 15, &control, query_current, &result) ==
        XR_OS_IO_CANCELLED && result && owner.handle && close_calls == before + 2);
    KILL_CHECK(xr_os_io_system_close(&owner) == XR_OS_IO_OK && !owner.handle);
    KILL_CHECK(WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT);
    result = false;
    close_failures = 1;
    before = close_calls;
    KILL_CHECK(xr_os_io_kill(&owner, process.dwProcessId, 15, NULL, NULL, &result) ==
        XR_OS_IO_IO && !result && owner.handle && close_calls == before + 1);
    KILL_CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD exit_code = 0;
    KILL_CHECK(GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 0xE0000001u);
    intptr_t saved = owner.handle;
    KILL_CHECK(xr_os_io_kill(&owner, process.dwProcessId, 15, NULL, NULL, &result) ==
        XR_OS_IO_BAD_ARGUMENT && owner.handle == saved && !result && close_calls == before + 1);
    KILL_CHECK(xr_os_io_system_close(&owner) == XR_OS_IO_OK && !owner.handle &&
        close_calls == before + 2);
    result = true;
    KILL_CHECK(xr_os_io_kill(&owner, process.dwProcessId, 15, NULL, NULL, &result) ==
        XR_OS_IO_OK && !result && !owner.handle);
    result = true;
    KILL_CHECK(xr_os_io_kill(&owner, -1, 15, NULL, NULL, &result) == XR_OS_IO_OK && !result);
    result = true;
    KILL_CHECK(xr_os_io_kill(&owner, INT64_MAX, 15, NULL, NULL, &result) == XR_OS_IO_OK && !result);
    result = true;
    KILL_CHECK(xr_os_io_kill(&owner, process.dwProcessId, INT64_MAX, NULL, NULL, &result) ==
        XR_OS_IO_OK && !result);
    KILL_CHECK(xr_os_io_kill(NULL, 1, 15, NULL, NULL, &result) == XR_OS_IO_BAD_ARGUMENT);
    KILL_CHECK(xr_os_io_kill(&owner, 1, 15, NULL, NULL, NULL) == XR_OS_IO_BAD_ARGUMENT);
    passed = 1;
cleanup:
    close_failures = 0;
    if (owner.handle) (void)xr_os_io_system_close(&owner);
    if (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
        (void)TerminateProcess(process.hProcess, 99);
        (void)WaitForSingleObject(process.hProcess, 10000);
    }
    if (!CloseHandle(process.hProcess)) passed = 0;
#undef KILL_CHECK
    return passed;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--sleep")) { Sleep(INFINITE); return 98; }
    if (argc == 3 && !strcmp(argv[1], "--parent")) {
        char *end = NULL;
        unsigned long parent = strtoul(argv[2], &end, 10);
        if (!parent || !end || *end) return 2;
        return verify_parent((int64_t)parent) ? 0 : 1;
    }
    if (argc != 1 || !verify_queries() || !verify_child_parent() || !verify_kill()) return 1;
    puts("system queries: identities, parent, memory, uptime, availability, close and kill");
    return 0;
}
#else
#include "os/unix/system_unix.c"

int main(void) {
    XrOsOptionalI64 uid = {0}, gid = {0};
    XrOsSystemOwner owner = {0};
    int64_t parent = -1, total = -1, available = -1;
    double uptime = -1;
    XrOsLoadAverages loads = {0};
    if (xr_os_io_uid(&uid) != XR_OS_IO_OK || !uid.present || uid.value != (int64_t)getuid() ||
        xr_os_io_gid(&gid) != XR_OS_IO_OK || !gid.present || gid.value != (int64_t)getgid() ||
        xr_os_io_ppid(&owner, NULL, NULL, &parent) != XR_OS_IO_OK || parent != (int64_t)getppid() ||
        xr_os_io_total_memory(&total) != XR_OS_IO_OK || total <= 0 ||
        xr_os_io_free_memory(&owner, &available) != XR_OS_IO_OK || owner.handle || available < 0 || available > total ||
        xr_os_io_uptime(&uptime) != XR_OS_IO_OK || uptime < 0 ||
        xr_os_io_loadavg(&loads) != XR_OS_IO_OK || !loads.available) return 1;
    for (size_t i = 0; i < 3; ++i) if (!(loads.values[i] >= 0.0)) return 1;
    puts("system queries: POSIX identities and actual system information");
    return 0;
}
#endif
