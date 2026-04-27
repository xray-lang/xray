/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xfd.h - Cross-platform file-descriptor accessors for stdin/out/err.
 *
 * KEY CONCEPT:
 *   POSIX hardcodes STDIN_FILENO / STDOUT_FILENO / STDERR_FILENO to
 *   0, 1, 2. Windows does not expose those macros; the equivalents
 *   are _fileno(stdin) etc., which are not constant. This header
 *   wraps the difference into three accessors plus a portable
 *   isatty helper so transports (LSP / DAP / MCP / CLI) do not have
 *   to embed `#ifdef _WIN32 #define STDIN_FILENO 0` blocks.
 *
 *   The accessors return integer fds and are intentionally cheap on
 *   both platforms (literal 0/1/2 on POSIX, single CRT call on
 *   Windows). Callers can pass the value to read/write/poll/dup
 *   exactly as they would the POSIX macro.
 *
 * RELATED MODULES:
 *   - base/xdefs.h   for visibility macros
 *   - base/xtime.h   for the parallel xtime portability layer
 */

#ifndef XFD_H
#define XFD_H

#include "xdefs.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// File-descriptor accessors. Cheap; safe to call repeatedly.
XR_FUNC int xr_stdin_fd(void);
XR_FUNC int xr_stdout_fd(void);
XR_FUNC int xr_stderr_fd(void);

// Returns true if the descriptor refers to a terminal. Wraps
// isatty() on POSIX and _isatty() on Windows.
XR_FUNC bool xr_isatty(int fd);

#ifdef __cplusplus
}
#endif

#endif  // XFD_H
