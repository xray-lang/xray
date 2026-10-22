/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * native_output_win.c - Raw Windows output without CRT newline translation
 *
 * KEY CONCEPT:
 *   WriteFile preserves LF bytes and captures its own Win32 error domain.
 */
#include "os/os_native_output.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static bool broken_pipe(DWORD code) {
    return code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ||
        code == ERROR_PIPE_NOT_CONNECTED;
}

XR_FUNCDEF XrtIoResult xrt_io_flush_stdout(void) {
    // UCRT maps a broken-pipe flush to EINVAL, so preserve its original code.
    (void)_set_doserrno(0);
    errno = 0;
    if (fflush(stdout) == 0) return (XrtIoResult){0};
    int code = errno;
    unsigned long windows_code = 0;
    if (_get_doserrno(&windows_code) == 0 && windows_code)
        return (XrtIoResult){true, broken_pipe(windows_code), XRT_IO_WINDOWS, windows_code};
    return (XrtIoResult){true, code == EPIPE,
        code ? XRT_IO_ERRNO : XRT_IO_NONE, (uint32_t)code};
}

XR_FUNCDEF XrtIoResult xrt_io_write(unsigned stream, const char *bytes, size_t length) {
    HANDLE handle = GetStdHandle(stream == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    while (length) {
        DWORD written = 0;
        DWORD amount = length > MAXDWORD ? MAXDWORD : (DWORD)length;
        if (!WriteFile(handle, bytes, amount, &written, NULL)) {
            DWORD code = GetLastError();
            return (XrtIoResult){true, broken_pipe(code), XRT_IO_WINDOWS, code};
        }
        if (!written) return (XrtIoResult){true, false, XRT_IO_NONE, 0};
        bytes += written;
        length -= written;
    }
    return (XrtIoResult){0};
}
