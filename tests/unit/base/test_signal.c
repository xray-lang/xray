/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_signal.c - Isolated persistent signal delivery and owner retirement
 */
#include "os/os_signal.h"
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "signal %d: %s\n", __LINE__, #condition); return 0; \
} } while (0)

#if defined(XR_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>

static bool fail_control, fail_console_list;
static DWORD injected_error;
static size_t add_attempts, remove_attempts;
static _Atomic(uint32_t) previous_hits;
static BOOL WINAPI observed_control(PHANDLER_ROUTINE handler, BOOL add) {
    if (add) ++add_attempts; else ++remove_attempts;
    if (fail_control) { fail_control = false; SetLastError(injected_error); return FALSE; }
    return SetConsoleCtrlHandler(handler, add);
}
static DWORD WINAPI observed_console_list(LPDWORD processes, DWORD capacity) {
    if (fail_console_list) { SetLastError(injected_error); return 0; }
    return GetConsoleProcessList(processes, capacity);
}
#define SetConsoleCtrlHandler observed_control
#define GetConsoleProcessList observed_console_list
#include "os/win/signal_win.c"
#undef GetConsoleProcessList
#undef SetConsoleCtrlHandler

static BOOL WINAPI previous_handler(DWORD event) {
    if (event != CTRL_C_EVENT) return FALSE;
    atomic_fetch_add_explicit(&previous_hits, 1, memory_order_relaxed);
    return TRUE;
}
static int wait_pending(XrOsSignalOwner *owner) {
    ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        bool pending = false;
        CHECK(xr_os_io_signal_take(owner, &pending) == XR_OS_IO_OK);
        if (pending) return 1;
        CHECK(GetTickCount64() < deadline);
        Sleep(1);
    }
}
static int wait_previous(uint32_t count) {
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (atomic_load_explicit(&previous_hits, memory_order_acquire) < count) {
        CHECK(GetTickCount64() < deadline);
        Sleep(1);
    }
    return 1;
}
static int console_controls(void) {
    DWORD clients[8] = {0};
    CHECK(GetConsoleProcessList(clients, 8) == 1 && clients[0] == GetCurrentProcessId());
    CHECK(SetConsoleCtrlHandler(previous_handler, TRUE));
    XrOsSignalOwner owner = {0}, competing = {0}; bool installed = true, pending = true;
    fail_console_list = true; injected_error = ERROR_NOT_ENOUGH_MEMORY;
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_OUT_OF_MEMORY && installed && signal_empty(&owner));
    injected_error = ERROR_SUCCESS;
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_IO && installed && signal_empty(&owner));
    fail_console_list = false; fail_control = true; injected_error = ERROR_NOT_ENOUGH_MEMORY;
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_OUT_OF_MEMORY && installed && signal_empty(&owner) &&
        !atomic_load_explicit(&signal_int_slot.word, memory_order_relaxed));
    fail_control = true; injected_error = ERROR_SUCCESS;
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_IO && installed && signal_empty(&owner));
    CHECK(xr_os_io_signal_open(15, &competing, &installed) == XR_OS_IO_OK && !installed && signal_empty(&competing));
    CHECK(xr_os_io_signal_open(9, &competing, &installed) == XR_OS_IO_OK && !installed);
    CHECK(xr_os_io_signal_open(-1, &competing, &installed) == XR_OS_IO_OK && !installed);
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_OK && installed && owner.token);
    CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && !pending);
    for (size_t delivery = 0; delivery < 2; ++delivery) {
        CHECK(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0));
        CHECK(wait_pending(&owner));
    }
    CHECK(!atomic_load_explicit(&previous_hits, memory_order_acquire));
    CHECK(signal_console_handler(CTRL_C_EVENT) && signal_console_handler(CTRL_C_EVENT));
    CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && pending);
    CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && !pending);
    CHECK(xr_os_io_signal_open(2, &competing, &installed) == XR_OS_IO_OK && !installed && signal_empty(&competing));
    XrOsSignalOwner copied = owner;
    installed = pending = true;
    CHECK(xr_os_io_signal_open(2, &copied, &installed) == XR_OS_IO_BAD_ARGUMENT && installed);
    CHECK(xr_os_io_signal_take(&copied, &pending) == XR_OS_IO_BAD_ARGUMENT && pending);
    CHECK(xr_os_io_signal_close(&copied) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(signal_console_handler(CTRL_C_EVENT));
    uint32_t old_word = atomic_load_explicit(&signal_int_slot.word, memory_order_relaxed);
    uint32_t old_generation = owner.generation; size_t adds = add_attempts;
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_OK && installed &&
        owner.generation > old_generation && add_attempts == adds);
    CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && !pending);
    CHECK(signal_console_handler(CTRL_C_EVENT));
    CHECK(!atomic_compare_exchange_strong_explicit(&signal_int_slot.word, &old_word, old_word | 1u,
        memory_order_relaxed, memory_order_relaxed));
    CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && pending);
    CHECK(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) && wait_pending(&owner));
    fail_control = true; injected_error = ERROR_SUCCESS;
    XrOsSignalOwner retained = owner;
    CHECK(xr_os_io_signal_close(&owner) == XR_OS_IO_IO && owner.token == retained.token &&
        owner.generation == retained.generation && !atomic_load_explicit(&signal_int_slot.word, memory_order_relaxed));
    pending = installed = true;
    CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_BAD_ARGUMENT && pending);
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_BAD_ARGUMENT && installed);
    CHECK(xr_os_io_signal_open(2, &competing, &installed) == XR_OS_IO_OK && !installed);
    CHECK(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) && wait_previous(1));
    size_t closes = remove_attempts;
    CHECK(xr_os_io_signal_close(&owner) == XR_OS_IO_OK && signal_empty(&owner) && remove_attempts == closes + 1);
    CHECK(xr_os_io_signal_close(&owner) == XR_OS_IO_OK && remove_attempts == closes + 1);
    CHECK(GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) && wait_previous(2));
    /* White-box identity exhaustion, not billions of registration attempts. */
    signal_epoch = UINT32_MAX >> 1; installed = true; adds = add_attempts;
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_OVERFLOW && installed &&
        signal_empty(&owner) && add_attempts == adds);
    CHECK(SetConsoleCtrlHandler(previous_handler, FALSE));
    CHECK(xr_os_io_signal_open(2, NULL, &installed) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_signal_take(NULL, &pending) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_os_io_signal_close(NULL) == XR_OS_IO_BAD_ARGUMENT);
    puts("signal: private console persistent INT, synthetic coalescing, owner/epoch, closing retry and prior handler");
    return 1;
}
static int detached_controls(void) {
    DWORD process = 0;
    CHECK(!GetConsoleProcessList(&process, 1));
    CHECK(GetLastError() == ERROR_INVALID_HANDLE);
    XrOsSignalOwner owner = {0}; bool installed = true;
    CHECK(xr_os_io_signal_open(2, &owner, &installed) == XR_OS_IO_OK && !installed && signal_empty(&owner));
    CHECK(xr_os_io_signal_open(15, &owner, &installed) == XR_OS_IO_OK && !installed && signal_empty(&owner));
    CHECK(xr_os_io_signal_close(&owner) == XR_OS_IO_OK);
    puts("signal: detached child has no console and installs no one-shot TERM handler");
    return 1;
}
static int run_isolated(const wchar_t *mode, DWORD flags) {
    wchar_t executable[32768], command[32768];
    DWORD length = GetModuleFileNameW(NULL, executable, 32768);
    CHECK(length && length < 32768);
    int count = swprintf(command, 32768, L"\"%ls\" %ls", executable, mode);
    CHECK(count > 0 && count < 32768);
    HANDLE handles[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    DWORD original_flags[3] = {0}; bool valid[3] = {false};
    for (size_t i = 0; i < 3; ++i) {
        if (handles[i] && handles[i] != INVALID_HANDLE_VALUE && GetHandleInformation(handles[i], &original_flags[i])) {
            valid[i] = true;
            CHECK(SetHandleInformation(handles[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
        }
    }
    STARTUPINFOW startup = {0}; startup.cb = (DWORD)sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES; startup.wShowWindow = SW_HIDE;
    startup.hStdInput = valid[0] ? handles[0] : NULL;
    startup.hStdOutput = valid[1] ? handles[1] : NULL;
    startup.hStdError = valid[2] ? handles[2] : NULL;
    PROCESS_INFORMATION process = {0};
    bool created = CreateProcessW(executable, command, NULL, NULL, TRUE, flags, NULL, NULL, &startup, &process) != 0;
    bool restored = true;
    for (size_t i = 0; i < 3; ++i)
        if (valid[i] && !SetHandleInformation(handles[i], HANDLE_FLAG_INHERIT, original_flags[i] & HANDLE_FLAG_INHERIT))
            restored = false;
    CHECK(created && restored);
    CHECK(CloseHandle(process.hThread));
    DWORD waited = WaitForSingleObject(process.hProcess, 20000), code = UINT32_MAX;
    bool queried = GetExitCodeProcess(process.hProcess, &code) != 0;
    if (waited != WAIT_OBJECT_0) { (void)TerminateProcess(process.hProcess, 99); (void)WaitForSingleObject(process.hProcess, 10000); }
    bool closed = CloseHandle(process.hProcess) != 0;
    if (code) fprintf(stderr, "signal child exit=%lu wait=%lu\n", (unsigned long)code, (unsigned long)waited);
    CHECK(waited == WAIT_OBJECT_0 && queried && !code && closed);
    return 1;
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--private-console")) return console_controls() ? 0 : 1;
    if (argc == 2 && !strcmp(argv[1], "--detached")) return detached_controls() ? 0 : 1;
    if (argc != 1) return 2;
    /* Never attach to or signal the coordinating process's console. */
    if (!run_isolated(L"--detached", DETACHED_PROCESS) ||
        !run_isolated(L"--private-console", CREATE_NEW_CONSOLE)) return 1;
    puts("signal: owned detached and hidden private-console children completed");
    return 0;
}
#else
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
static bool fail_action;
static int action_error;
static int observed_sigaction(int signal, const struct sigaction *action, struct sigaction *previous) {
    if (action && fail_action) { fail_action = false; errno = action_error; return -1; }
    return sigaction(signal, action, previous);
}
#define sigaction(...) observed_sigaction(__VA_ARGS__)
#include "os/unix/signal_unix.c"
#undef sigaction
static _Atomic(uint32_t) previous_hits;
static void previous_handler(int signal) { (void)signal; atomic_fetch_add_explicit(&previous_hits, 1, memory_order_relaxed); }
static int unix_controls(void) {
    const int signals[] = {2, 15};
    for (size_t i = 0; i < 2; ++i) {
        int native = signals[i] == 2 ? SIGINT : SIGTERM;
        struct sigaction prior = {0}, original;
        prior.sa_handler = previous_handler; CHECK(!sigemptyset(&prior.sa_mask));
        CHECK(!sigaction(native, &prior, &original));
        XrOsSignalOwner owner = {0}, competing = {0}; bool installed = true, pending = true;
        fail_action = true; action_error = ENOMEM;
        CHECK(xr_os_io_signal_open(signals[i], &owner, &installed) == XR_OS_IO_OUT_OF_MEMORY && installed && signal_empty(&owner));
        CHECK(xr_os_io_signal_open(signals[i], &owner, &installed) == XR_OS_IO_OK && installed);
        CHECK(!raise(native) && !raise(native));
        CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && pending);
        CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && !pending);
        CHECK(xr_os_io_signal_open(signals[i], &competing, &installed) == XR_OS_IO_OK && !installed);
        XrOsSignalOwner copied = owner;
        CHECK(xr_os_io_signal_close(&copied) == XR_OS_IO_BAD_ARGUMENT);
        CHECK(!raise(native)); uint32_t generation = owner.generation;
        CHECK(xr_os_io_signal_open(signals[i], &owner, &installed) == XR_OS_IO_OK && installed && owner.generation > generation);
        CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_OK && !pending);
        fail_action = true; action_error = 0;
        CHECK(xr_os_io_signal_close(&owner) == XR_OS_IO_IO && owner.token);
        CHECK(xr_os_io_signal_take(&owner, &pending) == XR_OS_IO_BAD_ARGUMENT);
        CHECK(xr_os_io_signal_close(&owner) == XR_OS_IO_OK && signal_empty(&owner));
        uint32_t hits = atomic_load_explicit(&previous_hits, memory_order_relaxed);
        CHECK(!raise(native) && atomic_load_explicit(&previous_hits, memory_order_relaxed) == hits + 1);
        CHECK(!sigaction(native, &original, NULL));
    }
    return 1;
}
int main(void) {
    pid_t child = fork(); if (child < 0) return 1;
    if (!child) _exit(unix_controls() ? 0 : 1);
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    puts("signal: isolated POSIX persistent INT/TERM and restored dispositions");
    return 0;
}
#endif
