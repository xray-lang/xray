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
#include <signal.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
#include <execinfo.h>
#define HAS_BACKTRACE 1
#endif

// GCC compatibility: __has_feature is Clang-specific
#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !(__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
static void crash_handler(int sig) {
    // Only use async-signal-safe functions: write(), _exit()
    const char *msg = "\n=== CRASH: signal unknown ===\n";
    if (sig == SIGSEGV) msg = "\n=== CRASH: SIGSEGV ===\n";
#ifdef SIGBUS
    else if (sig == SIGBUS) msg = "\n=== CRASH: SIGBUS ===\n";
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
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !(__has_feature(address_sanitizer) || __has_feature(thread_sanitizer))
    signal(SIGSEGV, crash_handler);
#ifdef SIGBUS
    signal(SIGBUS, crash_handler);
#endif
#endif
    xr_cli_register_all_handlers();
    return xr_cli_main(argc, argv);
}
