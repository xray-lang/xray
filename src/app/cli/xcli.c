/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcli.c - Xray command line thin entry point
 *
 * KEY CONCEPT:
 *   Only main() lives here. It installs the crash handler and delegates
 *   all routing to xr_cli_main() in xcli_dispatch.c.
 *   No command table, no arg_offset, no help printing.
 */

#include "xcli_dispatch.h"
#include "../../runtime/xr_process_shutdown.h"
#include "../../runtime/mem/xcycle_detector.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#if defined(_MSC_VER)
#include <corecrt_io.h>
#else
#include <io.h>
#endif
#include <process.h>
#include <Windows.h>
#include <crtdbg.h>
#else
#include <unistd.h>
#endif

#if defined(XR_OS_MACOS) || (defined(XR_OS_LINUX) && !defined(__ANDROID__))
#include <execinfo.h>
#define HAS_BACKTRACE 1
#endif

// GCC compatibility: __has_feature is Clang-specific
#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if !defined(XR_OS_WINDOWS) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) &&  \
    !(__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
static void crash_handler(int sig) {
    // Only use async-signal-safe functions: write(), _exit()
    const char *msg = "\n=== CRASH: signal unknown ===\n";
    if (sig == SIGSEGV)
        msg = "\n=== CRASH: SIGSEGV ===\n";
#ifdef SIGBUS
    else if (sig == SIGBUS)
        msg = "\n=== CRASH: SIGBUS ===\n";
#endif
    write(STDERR_FILENO, msg, strlen(msg));
#ifdef HAS_BACKTRACE
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
#endif
    _exit(128 + sig);
}
#endif

int main(int argc, char **argv) {
    atexit(xr_process_shutdown);
    /* MSVC's CRT (and most libc implementations) fully buffer stdout when
     * it is connected to a pipe — exactly the configuration the regression
     * runner uses. If a coroutine traps or aborts before the
     * test framework can flush, every "0301 + 1 passed" line stays in the
     * 4 KB user buffer and the dump that XRAY_TEST_DUMP_FAILED=1 prints
     * on CI is empty. Disable buffering on the standard streams so a
     * crashing process still hands its diagnostics to the parent shell.
     * stderr is unbuffered by default in C but MSVC redirects to fully
     * buffered behind a pipe; force _IONBF for both streams. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef XR_OS_WINDOWS
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#ifndef XR_OS_WINDOWS
    /* A networking runtime must never die because a peer closed the connection
     * mid-write. Ignore SIGPIPE process-wide so socket/pipe writes report EPIPE
     * through their return value (every send/write path already treats a failed
     * write as a normal I/O error). Set unconditionally — unlike the crash
     * handlers below this is safe and desired under sanitizers too. */
    signal(SIGPIPE, SIG_IGN);
#endif
#if !defined(XR_OS_WINDOWS) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) &&  \
    !(__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    signal(SIGSEGV, crash_handler);
#ifdef SIGBUS
    signal(SIGBUS, crash_handler);
#endif
#endif
    xr_cli_register_all_handlers();
    int rc = xr_cli_main(argc, argv);
#ifdef XR_ENABLE_CYCLE_DETECTOR
    /* Fail closed: a detected cycle is a non-zero exit even when the program
     * itself succeeded. A detector that finds leaks and still reports success
     * is a detector nobody acts on. Reported per coroutine as its heap is torn
     * down; the flag is accumulated so the run fails once, at the end. */
    if (rc == 0 && xr_cycle_detector_any_found())
        rc = 1;
#endif
    return rc;
}
