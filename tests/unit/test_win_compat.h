/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_win_compat.h - Suppress Windows CRT error dialogs in unit tests
 *
 * Include this and call xr_test_suppress_dialogs() at the top of main().
 * On non-Windows platforms this is a no-op.
 */

#ifndef XR_TEST_WIN_COMPAT_H
#define XR_TEST_WIN_COMPAT_H

#ifdef _WIN32
#include <stdlib.h>
#include <Windows.h>
#include <crtdbg.h>
#endif

static inline void xr_test_suppress_dialogs(void) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

#endif // XR_TEST_WIN_COMPAT_H
