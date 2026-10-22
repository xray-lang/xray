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
#include <windows.h>

XR_FUNCDEF XrtIoResult xrt_io_flush_stdout(void) {
    errno = 0;
    if (fflush(stdout) == 0) return (XrtIoResult){0};
    int code = errno;
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
            bool pipe = code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA ||
                code == ERROR_PIPE_NOT_CONNECTED;
            return (XrtIoResult){true, pipe, XRT_IO_WINDOWS, code};
        }
        if (!written) return (XrtIoResult){true, false, XRT_IO_NONE, 0};
        bytes += written;
        length -= written;
    }
    return (XrtIoResult){0};
}
