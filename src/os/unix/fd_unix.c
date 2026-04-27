/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfd_unix.c - POSIX implementation of xfd.h.
 *
 * STDIN_FILENO / STDOUT_FILENO / STDERR_FILENO are guaranteed by
 * POSIX to be 0/1/2; isatty is the canonical terminal check.
 */

#include "../os_fd.h"

#include <unistd.h>

int xr_stdin_fd(void) {
    return STDIN_FILENO;
}

int xr_stdout_fd(void) {
    return STDOUT_FILENO;
}

int xr_stderr_fd(void) {
    return STDERR_FILENO;
}

bool xr_isatty(int fd) {
    return isatty(fd) != 0;
}
