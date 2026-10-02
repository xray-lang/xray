/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * proc_self_win.c - Current-process queries without spawning dependencies
 */
#include "../os_proc.h"
#include "../../shared/xr_win_utf.h"

int64_t xr_proc_self_pid(void) {
    return (int64_t) GetCurrentProcessId();
}

int xr_proc_self_exe_path(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        return -1;
    }
    wchar_t wide[32768];
    DWORD n = GetModuleFileNameW(NULL, wide, (DWORD) (sizeof(wide) / sizeof(wide[0])));
    /* n == 0 means failure; n == capacity means truncation. */
    if (n == 0 || (size_t) n >= sizeof(wide) / sizeof(wide[0]) ||
        !xr_win_utf16_to_utf8(wide, (size_t) n, buf, size)) {
        return -1;
    }
    return 0;
}

bool xr_proc_debugger_attached(void) {
    return IsDebuggerPresent() ? true : false;
}
