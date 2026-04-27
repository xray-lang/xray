/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfd_win.c - Windows implementation of xfd.h.
 *
 * MSVCRT exposes _fileno(FILE*) and _isatty(int). The returned fd
 * is a CRT-level handle, suitable for _read/_write/_close calls
 * but distinct from a Win32 HANDLE (CreateFile result). All xray
 * transports that take a "stdin/out fd" treat it as a CRT fd, so
 * _fileno is the right choice.
 */

#include "../os_fd.h"

#include <io.h>
#include <stdio.h>

int xr_stdin_fd(void) {
    return _fileno(stdin);
}

int xr_stdout_fd(void) {
    return _fileno(stdout);
}

int xr_stderr_fd(void) {
    return _fileno(stderr);
}

bool xr_isatty(int fd) {
    return _isatty(fd) != 0;
}
